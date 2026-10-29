#include "schema/schema.hpp"

#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <functional>
#include <algorithm>

#include "schema/schema_data.hpp"
#include "util/util.hpp"

namespace docx::schema {

using namespace data;

std::string_view type_name_of(ValueKind k);

namespace {

const char* S(std::uint32_t i) { return strings[i]; }

std::string_view attr_qname(const RawAttr& a) {
    std::string_view q = S(a.qname);
    if (!q.empty() && q[0] == ':') q.remove_prefix(1);  // unqualified attribute
    return q;
}

// Hash that takes std::string_view too, so lookups don't allocate.
struct SvHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

struct Flat {
    // child qname -> (type, rank)
    std::unordered_map<std::string, std::pair<TypeId, int>, SvHash, std::equal_to<>> kids;
    std::vector<ChildInfo> order;
    bool has_any = false;
};

struct Index {
    std::unordered_map<std::string, std::vector<TypeId>> by_qname;
    std::unordered_map<std::string, TypeId> roots;
    std::unordered_map<std::string, std::string> uri_by_prefix, prefix_by_uri;
    std::vector<std::int32_t> particle;  // effective root particle per type
    std::vector<std::pair<std::uint32_t, std::uint32_t>> attr_range;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> text_vals;
    mutable std::unique_ptr<std::atomic<Flat*>[]> flat;
    mutable std::vector<std::unique_ptr<Flat>> flat_owned;
    mutable std::mutex mu;
};

const Index& idx() {
    static Index* I = [] {
        auto* ix = new Index;
        ix->particle.resize(n_types);
        ix->attr_range.resize(n_types);
        ix->text_vals.resize(n_types);
        ix->flat.reset(new std::atomic<Flat*>[n_types]);
        for (std::size_t i = 0; i < n_types; i++) ix->flat[i].store(nullptr, std::memory_order_relaxed);
        for (std::size_t i = 0; i < n_types; i++) {
            const RawType& t = types[i];
            if (t.qname) ix->by_qname[S(t.qname)].push_back(TypeId(i));
            std::int32_t p = t.particle;
            auto ar = std::make_pair(t.attr_begin, t.attr_end);
            auto tv = std::make_pair(t.val_begin, t.val_end);
            if (t.base >= 0) {
                const RawType& b = types[t.base];
                if (p < 0) p = b.particle;
                if (ar.first == ar.second) ar = {b.attr_begin, b.attr_end};
                if (tv.first == tv.second) tv = {b.val_begin, b.val_end};
            }
            ix->particle[i] = p;
            ix->attr_range[i] = ar;
            ix->text_vals[i] = tv;
        }
        for (std::size_t i = 0; i < n_parts; i++)
            if (parts[i].root >= 0) ix->roots.emplace(S(types[parts[i].root].qname), parts[i].root);
        for (std::size_t i = 0; i < n_namespaces; i++) {
            std::string p = S(namespaces[i].prefix), u = S(namespaces[i].uri);
            if (p.empty() && u.empty()) continue;
            ix->uri_by_prefix.emplace(p, u);
            ix->prefix_by_uri.emplace(u, p);
        }
        ix->uri_by_prefix["xml"] = "http://www.w3.org/XML/1998/namespace";
        return ix;
    }();
    return *I;
}

// Flatten a content model into child qname -> (type, rank). Children of a
// choice, or of anything repeatable, share a rank: they may interleave.
void flatten(std::int32_t pi, Flat& f, int& next, int shared) {
    const RawParticle& p = particles[pi];
    bool repeat = p.max != 1;
    if (p.kind == 0) {
        if (p.type < 0) return;
        int r = shared >= 0 ? shared : next++;
        std::string q = S(types[p.type].qname);
        if (!f.kids.count(q)) {
            f.kids.emplace(q, std::make_pair(TypeId(p.type), r));
            f.order.push_back({q, TypeId(p.type), p.min, p.max});
        }
        return;
    }
    if (p.kind == 5) { f.has_any = true; return; }
    int sh = shared;
    if (sh < 0 && (repeat || p.kind == 2 || p.kind == 4)) sh = next++;
    for (std::uint32_t c = p.child_begin; c < p.child_end; c++) flatten(particle_children[c], f, next, sh);
}

const Flat& flat_of(TypeId t) {
    const Index& I = idx();
    if (Flat* f = I.flat[t].load(std::memory_order_acquire)) return *f;
    std::lock_guard lk(I.mu);
    if (Flat* f = I.flat[t].load(std::memory_order_relaxed)) return *f;
    auto slot = std::make_unique<Flat>();
    int next = 0;
    if (I.particle[t] >= 0) flatten(I.particle[t], *slot, next, -1);
    Flat* raw = slot.get();
    I.flat_owned.push_back(std::move(slot));
    I.flat[t].store(raw, std::memory_order_release);
    return *raw;
}

std::string_view local(std::string_view q) {
    auto c = q.find(':');
    return c == std::string_view::npos ? q : q.substr(c + 1);
}
std::string_view prefix(std::string_view q) {
    auto c = q.find(':');
    return c == std::string_view::npos ? std::string_view{} : q.substr(0, c);
}

bool is_mc(std::string_view q) { return prefix(q) == "mc"; }

// The element an mc:AlternateContent stands for (first child of Fallback, or
// of the first Choice).
pugi::xml_node ac_proxy(pugi::xml_node ac) {
    for (auto c : ac.children()) {
        if (std::string_view(c.name()) == "mc:Fallback") {
            for (auto k : c.children()) if (k.type() == pugi::node_element) return k;
        }
    }
    for (auto c : ac.children()) {
        if (std::string_view(c.name()) == "mc:Choice")
            for (auto k : c.children()) if (k.type() == pugi::node_element) return k;
    }
    return {};
}

int rank_of(TypeId parent, pugi::xml_node child) {
    if (parent < 0) return -1;
    std::string_view n = child.name();
    if (n == "mc:AlternateContent") {
        auto p = ac_proxy(child);
        if (!p) return -1;
        n = p.name();
    }
    const Flat& f = flat_of(parent);
    auto it = f.kids.find(std::string(n));
    return it == f.kids.end() ? -1 : it->second.second;
}

// ── values ───────────────────────────────────────────────────────────────

bool parse_int(std::string_view s, long double& out) {
    s = std::string_view(s);
    std::string t = util::trim(s);
    if (t.empty()) return false;
    std::size_t i = 0;
    if (t[0] == '+' || t[0] == '-') i = 1;
    if (i == t.size()) return false;
    for (std::size_t k = i; k < t.size(); k++) if (t[k] < '0' || t[k] > '9') return false;
    out = std::strtold(t.c_str(), nullptr);
    return true;
}

bool parse_float(std::string_view s, long double& out) {
    std::string t = util::trim(s);
    if (t == "INF") { out = INFINITY; return true; }
    if (t == "-INF") { out = -INFINITY; return true; }
    if (t == "NaN") { out = NAN; return true; }
    if (t.empty()) return false;
    static const std::regex re(R"([+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?)");
    if (!std::regex_match(t, re)) return false;
    out = std::strtold(t.c_str(), nullptr);
    return true;
}

bool int_in(std::string_view s, long double lo, long double hi) {
    long double v;
    return parse_int(s, v) && v >= lo && v <= hi;
}

bool datetime_ok(std::string_view s) {
    static const std::regex re(
        R"(-?\d{4,}-\d{2}-\d{2}(T\d{2}:\d{2}(:\d{2}(\.\d+)?)?)?(Z|[+-]\d{2}:\d{2})?|\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})?|-?\d{4,}(-\d{2})?(Z|[+-]\d{2}:\d{2})?)");
    return std::regex_match(std::string(util::trim(s)), re);
}

bool enum_has(std::int32_t ei, std::string_view v) {
    if (ei < 0) return true;
    const RawEnum& e = enums[ei];
    for (auto f = e.facet_begin; f < e.facet_end; f++)
        if (v == S(enum_facets[f])) return true;
    return false;
}

std::vector<std::string_view> tokens(std::string_view s) {
    std::vector<std::string_view> v;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        std::size_t b = i;
        while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
        if (i > b) v.push_back(s.substr(b, i - b));
    }
    return v;
}

bool kind_ok(ValueKind k, std::int32_t ei, std::string_view v) {
    switch (k) {
        case ValueKind::String: return true;
        case ValueKind::Boolean: return v == "true" || v == "false" || v == "1" || v == "0";
        case ValueKind::OnOff:
            return v == "true" || v == "false" || v == "on" || v == "off" || v == "1" || v == "0";
        case ValueKind::TrueFalse: return v == "t" || v == "f" || v == "true" || v == "false";
        case ValueKind::TrueFalseBlank: return v.empty() || v == "t" || v == "f" || v == "true" || v == "false";
        case ValueKind::Byte: return int_in(v, 0, 255);
        case ValueKind::SByte: return int_in(v, -128, 127);
        case ValueKind::Int16: return int_in(v, -32768, 32767);
        case ValueKind::UInt16: return int_in(v, 0, 65535);
        case ValueKind::Int32: return int_in(v, -2147483648.0L, 2147483647.0L);
        case ValueKind::UInt32: return int_in(v, 0, 4294967295.0L);
        case ValueKind::Int64: return int_in(v, -9223372036854775808.0L, 9223372036854775807.0L);
        case ValueKind::UInt64: return int_in(v, 0, 18446744073709551615.0L);
        case ValueKind::Integer: { long double d; return parse_int(v, d); }
        case ValueKind::Decimal: {
            long double d;
            std::string t = util::trim(v);
            if (t == "INF" || t == "-INF" || t == "NaN") return false;
            return parse_float(v, d) && t.find_first_of("eE") == std::string::npos;
        }
        case ValueKind::Double:
        case ValueKind::Single: { long double d; return parse_float(v, d); }
        case ValueKind::HexBinary: {
            if (v.size() % 2) return false;
            for (char c : v) if (!std::isxdigit((unsigned char)c)) return false;
            return true;
        }
        case ValueKind::Base64Binary: return util::base64_decode(v).has_value();
        case ValueKind::DateTime: return datetime_ok(v);
        case ValueKind::Enum: return enum_has(ei, v);
        case ValueKind::List: return true;
    }
    return true;
}

bool number_ok(const RawValidator& rv, ValueKind own, std::string_view v) {
    long double d;
    bool parsed;
    switch (rv.num_kind) {
        case 1: parsed = parse_int(v, d); break;
        case 2: parsed = parse_int(v, d) && d >= 0; break;
        case 3: parsed = parse_float(v, d); break;
        case 4: return kind_ok(ValueKind::Boolean, -1, v);
        default:
            if (own == ValueKind::Double || own == ValueKind::Single || own == ValueKind::Decimal)
                parsed = parse_float(v, d);
            else if (own == ValueKind::String)
                parsed = parse_float(v, d);
            else
                parsed = kind_ok(own, -1, v) && parse_float(v, d);
    }
    if (!parsed) return false;
    if ((rv.flags & 1) && !(d >= (long double)rv.min)) return false;
    if ((rv.flags & 2) && !(d > (long double)rv.min)) return false;
    if ((rv.flags & 4) && !(d <= (long double)rv.max)) return false;
    if ((rv.flags & 8) && !(d < (long double)rv.max)) return false;
    return true;
}

bool ncname_ok(std::string_view s) {
    if (s.empty()) return false;
    auto u = util::to_u32(s);
    auto start = [](char32_t c) {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0;
    };
    if (!start(u[0])) return false;
    for (char32_t c : u)
        if (!(start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7)) return false;
    return true;
}

const std::regex& pattern(std::uint32_t si) {
    static std::mutex mu;
    static std::map<std::uint32_t, std::regex> cache;
    std::lock_guard lk(mu);
    auto it = cache.find(si);
    if (it == cache.end()) it = cache.emplace(si, std::regex(S(si), std::regex::ECMAScript)).first;
    return it->second;
}

bool string_ok(const RawValidator& rv, std::string_view v, bool is_list) {
    if (rv.flags & (1 << 5) || is_list) {
        for (auto t : tokens(v)) {
            RawValidator one = rv;
            one.flags &= ~(1 << 5);
            if (!string_ok(one, t, false)) return false;
        }
        return true;
    }
    std::size_t len = util::u32_length(v);
    if (rv.flags & (1 << 6)) {
        if (v.size() % 2) return false;
        for (char ch : v) if (!std::isxdigit((unsigned char)ch)) return false;
        len = v.size() / 2;
    }
    if (rv.length >= 0 && len != std::size_t(rv.length)) return false;
    if (rv.min_len >= 0 && len < std::size_t(rv.min_len)) return false;
    if (rv.max_len >= 0 && len > std::size_t(rv.max_len)) return false;
    if ((rv.flags & (1 << 1)) || (rv.flags & (1 << 3))) if (!ncname_ok(v)) return false;
    if (rv.flags & (1 << 2)) {
        auto c = v.find(':');
        if (c == std::string_view::npos ? !ncname_ok(v) : !(ncname_ok(v.substr(0, c)) && ncname_ok(v.substr(c + 1))))
            return false;
    }
    if (rv.pattern) {
        if (!std::regex_match(std::string(v), pattern(rv.pattern))) return false;
    }
    return true;
}

bool validator_ok(const RawValidator& rv, ValueKind own, std::int32_t own_enum, std::string_view v, bool is_list) {
    switch (rv.kind) {
        case 0: return true;  // required handled separately
        case 1: return string_ok(rv, v, false);
        case 2: {
            if ((rv.flags & 16) || is_list) {
                for (auto t : tokens(v)) if (!number_ok(rv, own, t)) return false;
                return true;
            }
            return number_ok(rv, own, v);
        }
        case 3: return enum_has(rv.enum_index, v);
    }
    return true;
}

// Check a value against kind + validators. Returns "" if fine.
std::string check_value(const RawAttr& a, std::string_view v) {
    ValueKind k = ValueKind(a.kind);
    bool is_list = k == ValueKind::List;
    if (is_list) {
        for (auto t : tokens(v))
            if (!kind_ok(ValueKind(a.item_kind), a.enum_index, t)) return "invalid list item '" + std::string(t) + "'";
    } else if (!kind_ok(k, a.enum_index, v)) {
        if (k == ValueKind::Enum) {
            std::string allowed;
            const RawEnum& e = enums[a.enum_index];
            for (auto f = e.facet_begin; f < e.facet_end && f < e.facet_begin + 40; f++)
                allowed += std::string(allowed.empty() ? "" : ", ") + S(enum_facets[f]);
            return "value '" + std::string(v) + "' is not one of: " + allowed;
        }
        return "value '" + std::string(v) + "' is not a valid " + std::string(type_name_of(k));
    }
    // Group by union id: plain validators must all pass, each union group needs one.
    std::map<int, bool> unions;
    for (auto i = a.val_begin; i < a.val_end; i++) {
        const RawValidator& rv = validators[i];
        if (rv.kind == 0) continue;
        bool ok;
        if (k == ValueKind::HexBinary && rv.kind == 1) {
            RawValidator hv = rv;
            hv.flags |= 1 << 6;
            ok = string_ok(hv, v, false);
        } else {
            ok = validator_ok(rv, k, a.enum_index, v, is_list);
        }
        if (rv.union_id < 0) {
            if (!ok) return "value '" + std::string(v) + "' violates a constraint";
        } else {
            unions[rv.union_id] = unions[rv.union_id] || ok;
        }
    }
    for (auto& [id, ok] : unions)
        if (!ok) return "value '" + std::string(v) + "' matches none of the allowed forms";
    return "";
}

}  // namespace

std::string_view type_name_of(ValueKind k) {
    switch (k) {
        case ValueKind::Boolean: return "boolean";
        case ValueKind::OnOff: return "on/off value";
        case ValueKind::TrueFalse:
        case ValueKind::TrueFalseBlank: return "t/f value";
        case ValueKind::DateTime: return "dateTime";
        case ValueKind::HexBinary: return "hexBinary";
        case ValueKind::Base64Binary: return "base64Binary";
        case ValueKind::Double:
        case ValueKind::Single:
        case ValueKind::Decimal: return "number";
        case ValueKind::String: return "string";
        default: return "integer in range";
    }
}

std::string_view uri_of_prefix(std::string_view p) {
    auto& m = idx().uri_by_prefix;
    auto it = m.find(std::string(p));
    return it == m.end() ? std::string_view{} : std::string_view(it->second);
}
std::string_view prefix_of_uri(std::string_view u) {
    auto& m = idx().prefix_by_uri;
    auto it = m.find(std::string(u));
    return it == m.end() ? std::string_view{} : std::string_view(it->second);
}
bool namespace_known(std::string_view uri) { return !prefix_of_uri(uri).empty(); }

TypeId root_type(std::string_view q) {
    auto& I = idx();
    if (auto it = I.roots.find(std::string(q)); it != I.roots.end()) return it->second;
    if (auto it = I.by_qname.find(std::string(q)); it != I.by_qname.end() && it->second.size() == 1)
        return it->second[0];
    return NoType;
}

TypeId part_root_type(std::string_view ct) {
    // Generic XML parts (custom XML data, ribbon) are typed by relationship, not content type.
    if (ct == "application/xml" || ct == "text/xml") return NoType;
    // The main part's content type varies with the file kind (docx/docm/dotx/dotm).
    static const char* mains[] = {
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
        "application/vnd.ms-word.document.macroEnabled.main+xml",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml",
        "application/vnd.ms-word.template.macroEnabledTemplate.main+xml"};
    for (auto m : mains)
        if (ct == m) return root_type("w:document");
    for (std::size_t i = 0; i < n_parts; i++)
        if (parts[i].root >= 0 && ct == S(parts[i].content_type)) return parts[i].root;
    return NoType;
}

TypeId child_type(TypeId parent, std::string_view q) {
    if (parent < 0) return NoType;
    const Flat& f = flat_of(parent);
    auto it = f.kids.find(q);
    return it == f.kids.end() ? NoType : it->second.first;
}

std::string_view type_key(TypeId t) { return t < 0 ? "" : S(types[t].key); }
std::string_view type_qname(TypeId t) { return t < 0 ? "" : S(types[t].qname); }
std::string_view type_summary(TypeId t) { return t < 0 ? "" : S(types[t].summary); }
bool is_leaf_text(TypeId t) { return t >= 0 && (types[t].flags & 2); }

std::vector<ChildInfo> children_of(TypeId t) {
    if (t < 0) return {};
    return flat_of(t).order;
}

std::vector<AttrInfo> attributes_of(TypeId t) {
    std::vector<AttrInfo> v;
    if (t < 0) return v;
    auto [b, e] = idx().attr_range[t];
    for (auto i = b; i < e; i++) {
        const RawAttr& a = attrs[i];
        AttrInfo ai;
        ai.qname = attr_qname(a);
        ai.kind = ValueKind(a.kind);
        if (a.enum_index >= 0) {
            const RawEnum& en = enums[a.enum_index];
            for (auto f = en.facet_begin; f < en.facet_end; f++) ai.enum_values.push_back(S(enum_facets[f]));
        }
        for (auto k = a.val_begin; k < a.val_end; k++)
            if (validators[k].kind == 0 && validators[k].flags) ai.required = true;
        v.push_back(std::move(ai));
    }
    return v;
}

int child_rank(TypeId parent, std::string_view q) {
    if (parent < 0) return -1;
    const Flat& f = flat_of(parent);
    auto it = f.kids.find(std::string(q));
    return it == f.kids.end() ? -1 : it->second.second;
}

TypeId type_of(pugi::xml_node n) {
    if (!n || n.type() != pugi::node_element) return NoType;
    std::vector<pugi::xml_node> chain;
    for (auto c = n; c && c.type() == pugi::node_element; c = c.parent()) chain.push_back(c);
    TypeId t = root_type(chain.back().name());
    for (std::size_t i = chain.size() - 1; i-- > 0;) {
        std::string_view nm = chain[i].name();
        if (is_mc(nm)) continue;  // AlternateContent/Choice/Fallback are transparent
        t = child_type(t, nm);
        if (t < 0) return NoType;
    }
    return t;
}

namespace {

pugi::xml_node insert_at_rank(pugi::xml_node parent, TypeId pt, std::string_view q) {
    int r = child_rank(pt, q);
    pugi::xml_node after;
    int last = -1;
    for (auto c : parent.children()) {
        if (c.type() != pugi::node_element) continue;
        int cr = rank_of(pt, c);
        if (cr < 0) cr = last;  // unknown: rides with its predecessor
        if (r >= 0 && cr > r) break;
        after = c;
        last = cr;
    }
    if (r < 0) return parent.append_child(std::string(q).c_str());
    if (!after) return parent.prepend_child(std::string(q).c_str());
    return parent.insert_child_after(std::string(q).c_str(), after);
}

}  // namespace

pugi::xml_node ensure_child(pugi::xml_node parent, std::string_view q) {
    if (auto c = parent.child(std::string(q).c_str())) return c;
    return insert_at_rank(parent, type_of(parent), q);
}

pugi::xml_node insert_child(pugi::xml_node parent, std::string_view q) {
    return insert_at_rank(parent, type_of(parent), q);
}

void place_child(pugi::xml_node parent, pugi::xml_node child) {
    TypeId pt = type_of(parent);
    int r = rank_of(pt, child);
    if (r < 0) return;
    pugi::xml_node after;
    int last = -1;
    for (auto c : parent.children()) {
        if (c == child || c.type() != pugi::node_element) continue;
        int cr = rank_of(pt, c);
        if (cr < 0) cr = last;
        if (cr > r) break;
        after = c;
        last = cr;
    }
    if (after) parent.insert_move_after(child, after);
    else parent.prepend_move(child);
}

void sort_children(pugi::xml_node parent) {
    TypeId pt = type_of(parent);
    if (pt < 0) return;
    std::vector<std::pair<int, pugi::xml_node>> v;
    int last = -1;
    bool sorted = true;
    for (auto c : parent.children()) {
        int r = c.type() == pugi::node_element ? rank_of(pt, c) : -1;
        if (r < 0) r = last;
        if (r < last) sorted = false;
        v.emplace_back(r, c);
        last = r;
    }
    if (sorted) return;
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [r, c] : v) parent.append_move(c);
}

// ── validation ───────────────────────────────────────────────────────────

namespace {

struct Ctx {
    std::vector<Issue>* out;
    const ValidateOptions* opt;
};

using Ign = std::vector<std::string>;  // ignorable prefixes in scope

std::string path_of(pugi::xml_node n) {
    std::vector<std::string> segs;
    for (auto c = n; c && c.type() == pugi::node_element; c = c.parent()) {
        int i = 1;
        for (auto s = c.previous_sibling(c.name()); s; s = s.previous_sibling(c.name())) i++;
        segs.push_back(std::string(c.name()) + "[" + std::to_string(i) + "]");
    }
    std::string p;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) p += "/" + *it;
    return p;
}

void add(Ctx& c, pugi::xml_node n, std::string msg) {
    if (c.out->size() < c.opt->max_issues) c.out->push_back({path_of(n), std::move(msg)});
}

// Element sequence matcher (greedy, like the SDK's particle validators).
// FIRST(p): the element names an occurrence of particle p can start with, and
// whether p can match nothing. Lets once() reject a branch without walking it.
struct First {
    std::vector<std::string_view> names;  // sorted
    bool any = false;    // has an xsd:any somewhere at its start
    bool empty = false;  // can match zero elements
};

const First& first_of(std::int32_t pi);

First compute_first(std::int32_t pi) {
    const RawParticle& p = particles[pi];
    First f;
    auto merge = [&](const First& o) {
        f.names.insert(f.names.end(), o.names.begin(), o.names.end());
        f.any = f.any || o.any;
    };
    switch (p.kind) {
        case 0:
            if (p.type >= 0) f.names.push_back(S(types[p.type].qname));
            break;
        case 5: f.any = true; break;
        case 1:
        case 3: {
            f.empty = true;
            for (auto c = p.child_begin; c < p.child_end; c++) {
                const First& k = first_of(particle_children[c]);
                bool kid_empty = k.empty || particles[particle_children[c]].min == 0;
                merge(k);
                if (!kid_empty) { f.empty = false; break; }
            }
            break;
        }
        case 2:
        case 4: {
            f.empty = p.child_begin == p.child_end || p.kind == 4;
            for (auto c = p.child_begin; c < p.child_end; c++) {
                const First& k = first_of(particle_children[c]);
                merge(k);
                if (k.empty || particles[particle_children[c]].min == 0) f.empty = true;
            }
            if (p.kind == 4) {
                // all: empty only when every member is optional
                f.empty = true;
                for (auto c = p.child_begin; c < p.child_end; c++)
                    if (particles[particle_children[c]].min > 0 && !first_of(particle_children[c]).empty) f.empty = false;
            }
            break;
        }
        default: f.any = true; f.empty = true;
    }
    std::sort(f.names.begin(), f.names.end());
    f.names.erase(std::unique(f.names.begin(), f.names.end()), f.names.end());
    return f;
}

const First& first_of(std::int32_t pi) {
    static std::mutex mu;
    static std::unique_ptr<std::atomic<First*>[]> cache = [] {
        std::unique_ptr<std::atomic<First*>[]> c(new std::atomic<First*>[n_particles]);
        for (std::size_t i = 0; i < n_particles; i++) c[i].store(nullptr, std::memory_order_relaxed);
        return c;
    }();
    if (First* f = cache[pi].load(std::memory_order_acquire)) return *f;
    // mark in progress with an "anything goes" entry so recursive models terminate
    static First wildcard{{}, true, true};
    {
        std::lock_guard lk(mu);
        if (First* f = cache[pi].load(std::memory_order_relaxed)) return *f;
        cache[pi].store(&wildcard, std::memory_order_release);
    }
    auto* f = new First(compute_first(pi));
    cache[pi].store(f, std::memory_order_release);
    return *f;
}

struct Matcher {
    const std::vector<std::pair<std::string, pugi::xml_node>>& kids;
    // once() results for (particle, position); a match is a pure function of these
    mutable std::unordered_map<std::uint64_t, long> memo;

    // Can particle pi start at pos? false means it certainly can't consume kids[pos].
    bool may_start(std::int32_t pi, long pos) const {
        if (pos >= (long)kids.size()) return true;
        const First& f = first_of(pi);
        if (f.any) return true;
        return std::binary_search(f.names.begin(), f.names.end(), std::string_view(kids[pos].first));
    }

    bool any_ok(std::uint8_t ns, std::string_view q, TypeId parent) const {
        std::string_view pre = prefix(q);
        std::string_view parent_pre = prefix(type_qname(parent));
        switch (ns) {
            case 0: return true;
            case 1: return pre != parent_pre && !pre.empty();
            case 2: return pre.empty();
            case 3: return pre == parent_pre;
        }
        return true;
    }

    // Try to match one occurrence of particle p at pos. Returns new pos or -1.
    long once(std::int32_t pi, long pos, TypeId parent) const {
        const RawParticle& p = particles[pi];
        if (p.kind == 0 || p.kind == 5) return once_raw(pi, pos, parent);
        std::uint64_t key = (std::uint64_t(std::uint32_t(pi)) << 32) | std::uint32_t(pos);
        if (auto it = memo.find(key); it != memo.end()) return it->second;
        long r = once_raw(pi, pos, parent);
        memo.emplace(key, r);
        return r;
    }

    long once_raw(std::int32_t pi, long pos, TypeId parent) const {
        const RawParticle& p = particles[pi];
        switch (p.kind) {
            case 0: {
                if (pos < (long)kids.size() && p.type >= 0 && kids[pos].first == S(types[p.type].qname)) return pos + 1;
                return -1;
            }
            case 5: {
                if (pos < (long)kids.size() && any_ok(p.any_ns, kids[pos].first, parent)) return pos + 1;
                return -1;
            }
            case 1:
            case 3: {
                long cur = pos;
                for (auto c = p.child_begin; c < p.child_end; c++) {
                    long n = occurs(particle_children[c], cur, parent);
                    if (n < 0) return -1;
                    cur = n;
                }
                return cur;
            }
            case 2: {
                long best = -1;
                // try branches that can start with this element first; the empty ones only if none takes it
                bool at_end = pos >= (long)kids.size();
                for (int pass = 0; pass < 2; pass++) {
                    for (auto c = p.child_begin; c < p.child_end; c++) {
                        std::int32_t ci = particle_children[c];
                        bool starts = at_end || may_start(ci, pos);
                        if (pass == 0 && !starts) continue;
                        if (pass == 1 && (starts || !(first_of(ci).empty || particles[ci].min == 0))) continue;
                        long n = occurs(ci, pos, parent);
                        if (n > best) best = n;
                        if (n > pos) return n;
                    }
                    if (best >= 0 && pass == 0 && !at_end) {
                        // a starting branch matched empty; an empty-only branch can't do better
                        return best;
                    }
                }
                return best;
            }
            case 4: {
                long cur = pos;
                std::vector<bool> used(p.child_end - p.child_begin, false);
                bool progress = true;
                while (progress) {
                    progress = false;
                    for (auto c = p.child_begin; c < p.child_end; c++) {
                        if (used[c - p.child_begin]) continue;
                        long n = once(particle_children[c], cur, parent);
                        if (n > cur) { used[c - p.child_begin] = true; cur = n; progress = true; }
                    }
                }
                for (auto c = p.child_begin; c < p.child_end; c++)
                    if (!used[c - p.child_begin] && particles[particle_children[c]].min > 0) return -1;
                return cur;
            }
        }
        return -1;
    }

    long occurs(std::int32_t pi, long pos, TypeId parent) const {
        const RawParticle& p = particles[pi];
        long cur = pos;
        std::uint32_t count = 0;
        while (p.max == 0 || count < p.max) {
            // nothing more of this particle can start here: stop without trying
            if (!may_start(pi, cur) && !first_of(pi).empty) break;
            long n = once(pi, cur, parent);
            if (n < 0) break;
            if (n == cur) { count = std::max<std::uint32_t>(count, p.min); break; }
            cur = n;
            count++;
        }
        if (count < p.min) {
            // An all-optional body that matched nothing still satisfies min.
            long z = once(pi, cur, parent);
            if (z == cur) return cur;
            return -1;
        }
        return cur;
    }
};

bool ignorable(const Ign& ign, std::string_view q) {
    std::string_view pre = prefix(q);
    if (pre.empty()) return false;
    for (auto& p : ign) if (p == pre) return true;
    return false;
}

// Ignorable scope after applying n's own mc:Ignorable.
Ign scope_of(const Ign& outer, pugi::xml_node n) {
    auto a = n.attribute("mc:Ignorable");
    if (!a) return outer;
    Ign s = outer;
    for (auto& t : util::split(util::trim(a.value()), ' ')) if (!t.empty()) s.push_back(t);
    return s;
}

void validate_node(Ctx& c, pugi::xml_node n, TypeId t, const Ign& outer);

void validate_attrs(Ctx& c, pugi::xml_node n, TypeId t, const Ign& ign) {
    auto [b, e] = idx().attr_range[t];
    for (auto a : n.attributes()) {
        std::string_view q = a.name();
        if (q == "xmlns" || util::starts_with(q, "xmlns:")) continue;
        if (is_mc(q)) continue;
        bool found = false;
        for (auto i = b; i < e; i++) {
            if (attr_qname(attrs[i]) != q) continue;
            found = true;
            std::string why = check_value(attrs[i], a.value());
            if (!why.empty()) add(c, n, "attribute " + std::string(q) + ": " + why);
            break;
        }
        if (!found && !ignorable(ign, q)) {
            std::string_view pre = prefix(q);
            if (pre != "xml")
                add(c, n, "unexpected attribute " + std::string(q) + " on " + std::string(n.name()));
        }
    }
    for (auto i = b; i < e; i++) {
        const RawAttr& ra = attrs[i];
        bool req = false;
        for (auto k = ra.val_begin; k < ra.val_end; k++)
            if (validators[k].kind == 0) req = validators[k].flags != 0;
        if (req && !n.attribute(std::string(attr_qname(ra)).c_str()))
            add(c, n, "missing required attribute " + std::string(attr_qname(ra)));
    }
}

// The branch of an AlternateContent that a Microsoft365 consumer takes: the
// first Choice whose Requires prefixes are all known namespaces, else Fallback.
pugi::xml_node ac_select(pugi::xml_node ac) {
    for (auto ch : ac.children()) {
        if (std::string_view(ch.name()) != "mc:Choice") continue;
        std::string req = util::trim(ch.attribute("mc:Requires").value());
        if (req.empty()) req = util::trim(ch.attribute("Requires").value());
        if (req.empty()) continue;
        bool ok = true;
        for (auto& p : util::split(req, ' ')) {
            if (p.empty()) continue;
            if (uri_of_prefix(p).empty()) { ok = false; break; }
        }
        if (ok) return ch;
    }
    for (auto ch : ac.children())
        if (std::string_view(ch.name()) == "mc:Fallback") return ch;
    return {};
}

struct Kid {
    std::string_view q;
    pugi::xml_node n;
    const Ign* ign;  // scope the child lives in (points at the parent's scope unless the child declares its own)
    std::shared_ptr<Ign> own;
};

// Logical children after MCE preprocessing: ignorable elements skipped,
// AlternateContent replaced by its selected branch's content.
void collect_kids(pugi::xml_node n, TypeId t, const Ign& ign, std::vector<Kid>& kids) {
    for (auto k : n.children()) {
        if (k.type() != pugi::node_element) continue;
        std::string_view q = k.name();
        if (q == "mc:AlternateContent") {
            Ign s = scope_of(ign, k);
            auto br = ac_select(k);
            if (br) {
                // branch children need a scope that outlives this call
                auto sc = std::make_shared<Ign>(scope_of(s, br));
                std::size_t before = kids.size();
                collect_kids(br, t, *sc, kids);
                for (std::size_t i = before; i < kids.size(); i++)
                    if (kids[i].ign == sc.get()) kids[i].own = sc;
            }
            continue;
        }
        Kid kid{q, k, &ign, nullptr};
        if (k.attribute("mc:Ignorable")) {
            kid.own = std::make_shared<Ign>(scope_of(ign, k));
            kid.ign = kid.own.get();
        }
        // Like the SDK: an element the parent's model doesn't know is skipped
        // when its namespace is ignorable; known ones are always validated.
        if (child_type(t, q) < 0 && ignorable(*kid.ign, q)) continue;
        kids.push_back(std::move(kid));
    }
}

void validate_node(Ctx& c, pugi::xml_node n, TypeId t, const Ign& ign) {
    if (c.out->size() >= c.opt->max_issues) return;
    validate_attrs(c, n, t, ign);
    const RawType& rt = types[t];
    if (rt.flags & 2) {
        // leaf text: check text validators
        auto [vb, ve] = idx().text_vals[t];
        for (auto i = vb; i < ve; i++) {
            const RawValidator& rv = validators[i];
            if (rv.kind == 0) continue;
            if (!validator_ok(rv, ValueKind::String, -1, n.text().get(), false))
                add(c, n, "text content '" + std::string(n.text().get()) + "' is not valid");
        }
        for (auto k : n.children())
            if (k.type() == pugi::node_element) add(c, k, "element not allowed inside text element");
        return;
    }
    std::vector<Kid> kids;
    collect_kids(n, t, ign, kids);
    std::vector<std::pair<std::string, pugi::xml_node>> known;
    known.reserve(kids.size());
    for (auto& k : kids) known.emplace_back(std::string(k.q), k.n);
    std::int32_t pi = idx().particle[t];
    if (pi < 0) {
        if (!known.empty()) add(c, known[0].second, "element " + known[0].first + " is not allowed in " + std::string(n.name()));
        return;
    }
    Matcher m{known};
    long end = m.occurs(pi, 0, t);
    if (end < 0 || end < (long)known.size()) {
        // Point at the first unknown or out-of-order child.
        const Flat& f = flat_of(t);
        bool reported = false;
        int last_rank = -1;
        for (auto& k : known) {
            auto it = f.kids.find(k.first);
            if (it == f.kids.end()) {
                add(c, k.second, "element " + k.first + " is not allowed in " + std::string(n.name()));
                reported = true;
                break;
            }
            if (it->second.second < last_rank) {
                add(c, k.second, "element " + k.first + " is out of order in " + std::string(n.name()));
                reported = true;
                break;
            }
            last_rank = it->second.second;
        }
        if (!reported) add(c, n, "content of " + std::string(n.name()) + " does not match its schema (missing required child or too many occurrences)");
    }
    for (auto& k : kids) {
        TypeId ct = child_type(t, k.q);
        if (ct >= 0) validate_node(c, k.n, ct, *k.ign);
    }
}

// The SDK's semantic constraints. Element-level ones (ranges, lengths,
// sets, patterns, mutual exclusion) and the part-level uniqueness rule.
void validate_semantic(Ctx& c, pugi::xml_node root) {
    // index constraints by element qname
    static const std::unordered_map<std::string, std::vector<const RawSemantic*>> by_elem = [] {
        std::unordered_map<std::string, std::vector<const RawSemantic*>> m;
        for (std::size_t i = 0; i < n_semantics; i++) {
            const RawSemantic& s = semantics[i];
            if (s.version > 7) continue;
            m[S(s.elem)].push_back(&s);
        }
        return m;
    }();
    auto local_eq = [](std::string_view a, std::string_view b) { return local(a) == local(b); };
    auto find_attr = [&](pugi::xml_node n, std::string_view q) -> pugi::xml_attribute {
        // the SDK matches by local name (some constraints carry the wrong namespace)
        pugi::xml_attribute loose;
        for (auto a : n.attributes()) {
            if (std::string_view(a.name()) == q) return a;
            if (!loose && local_eq(a.name(), q) && std::string_view(a.name()).find("xmlns") != 0) loose = a;
        }
        return loose;
    };
    // uniqueness: (elem, attr) -> value -> count, over the whole part
    std::map<std::pair<std::string, std::string>, std::map<std::string, int>> uniq;
    std::vector<std::pair<pugi::xml_node, const RawSemantic*>> uniq_hits;
    std::function<void(pugi::xml_node, TypeId)> walk = [&](pugi::xml_node n, TypeId nt) {
        for (auto k : n.children()) {
            if (k.type() != pugi::node_element) continue;
            std::string_view q = k.name();
            if (q == "mc:Fallback") continue;  // the SDK reads the selected branch only
            TypeId kt = is_mc(q) ? nt : child_type(nt, q);
            if (is_mc(q)) { walk(k, nt); continue; }
            if (kt < 0) continue;
            std::string key(type_key(kt));
            if (auto it = by_elem.find(key); it != by_elem.end()) {
                for (const RawSemantic* s : it->second) {
                    std::string aq = S(s->attr);
                    if (s->kind == 5) {
                        int present = 0;
                        for (auto& name : util::split(aq, '\x1f'))
                            if (find_attr(k, name)) present++;
                        if (present > 1) add(c, k, "attributes " + aq + " are mutually exclusive");
                        continue;
                    }
                    auto a = find_attr(k, aq);
                    if (!a) continue;
                    std::string v = a.value();
                    switch (s->kind) {
                        case 0:
                            // the SDK skips constraints scoped to a parent (`if (_parent is not null) return null;`)
                            if (v.empty() || s->extra) break;
                            uniq[{key, aq}][s->f1 ? v : util::lower(v)]++;
                            uniq_hits.push_back({k, s});
                            break;
                        case 1: {
                            if (v.empty()) break;
                            long double d;
                            // like GetAttrNumVal: hexBinary values parse as hex
                            bool hexv = !v.empty() && std::all_of(v.begin(), v.end(), [](char ch) { return std::isxdigit((unsigned char)ch); }) &&
                                        std::any_of(v.begin(), v.end(), [](char ch) { return std::isalpha((unsigned char)ch); });
                            // VML fixed point: "22938f" is 22938/65536
                            bool fixed = v.size() > 1 && v.back() == 'f' &&
                                         std::all_of(v.begin(), v.end() - 1, [](char ch) { return std::isdigit((unsigned char)ch) || ch == '-'; });
                            if (fixed) d = std::strtold(v.c_str(), nullptr) / 65536.0L;
                            else if (hexv) d = (long double)std::stoull(v, nullptr, 16);
                            else if (!parse_float(v, d)) break;
                            long double lo = std::strtold(S(s->a), nullptr), hi = std::strtold(S(s->b), nullptr);
                            bool ok = (s->f1 ? d >= lo : d > lo) && (s->f2 ? d <= hi : d < hi);
                            if (!ok) add(c, k, "attribute " + aq + ": value " + v + " is out of range [" + S(s->a) + ", " + S(s->b) + "]");
                            break;
                        }
                        case 2: {
                            auto len = (long)util::u32_length(v);
                            if (len < std::stol(S(s->a)) || len > std::stol(S(s->b)))
                                add(c, k, "attribute " + aq + ": length " + std::to_string(len) + " outside " + S(s->a) + ".." + S(s->b));
                            break;
                        }
                        case 3: {
                            if (v.empty()) break;
                            bool in = false;
                            for (auto& x : util::split(S(s->extra), '\x1f')) if (x == v) in = true;
                            if (in != (s->f1 != 0)) add(c, k, "attribute " + aq + ": value '" + v + "' is not allowed here");
                            break;
                        }
                        case 4: {
                            if (v.empty()) break;
                            try {
                                // .NET IsMatch: anywhere in the string unless anchored
                                if (!std::regex_search(v, std::regex(S(s->extra)))) add(c, k, "attribute " + aq + ": value '" + v + "' does not match " + S(s->extra));
                            } catch (std::regex_error&) {}
                            break;
                        }
                    }
                }
            }
            walk(k, kt);
        }
    };
    TypeId rt = root_type(root.name());
    walk(root, rt);
    for (auto& [n, s] : uniq_hits) {
        std::string aq = S(s->attr);
        std::string v = find_attr(n, aq).value();
        if (uniq[{std::string(S(s->elem)), aq}][s->f1 ? v : util::lower(v)] > 1)
            add(c, n, "attribute " + aq + " should be unique; value '" + v + "' is used more than once");
    }
}

}  // namespace

std::vector<Issue> validate(pugi::xml_node root, const ValidateOptions& opt) {
    std::vector<Issue> out;
    Ctx c{&out, &opt};
    TypeId t = root_type(root.name());
    if (t < 0) {
        out.push_back({"/" + std::string(root.name()), "unknown root element"});
        return out;
    }
    validate_node(c, root, t, scope_of({}, root));
    if (opt.semantic) validate_semantic(c, root);
    return out;
}

bool value_valid(TypeId t, std::string_view q, std::string_view v, std::string* why) {
    if (t < 0) return true;
    auto [b, e] = idx().attr_range[t];
    for (auto i = b; i < e; i++) {
        if (attr_qname(attrs[i]) != q) continue;
        std::string w = check_value(attrs[i], v);
        if (why) *why = w;
        return w.empty();
    }
    if (why) *why = "unknown attribute";
    return false;
}

}  // namespace docx::schema
