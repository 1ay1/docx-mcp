#include "xml/xml.hpp"

#include <map>
#include <set>
#include <sstream>

#include "schema/schema.hpp"
#include "util/util.hpp"

namespace docx::xml {

namespace {

// Strict (ISO 29500 Strict) namespaces -> Transitional equivalents.
const std::map<std::string, std::string>& strict_map() {
    static const std::map<std::string, std::string> m = {
        {"http://purl.oclc.org/ooxml/wordprocessingml/main", "http://schemas.openxmlformats.org/wordprocessingml/2006/main"},
        {"http://purl.oclc.org/ooxml/officeDocument/relationships", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"},
        {"http://purl.oclc.org/ooxml/drawingml/main", "http://schemas.openxmlformats.org/drawingml/2006/main"},
        {"http://purl.oclc.org/ooxml/drawingml/wordprocessingDrawing", "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"},
        {"http://purl.oclc.org/ooxml/drawingml/picture", "http://schemas.openxmlformats.org/drawingml/2006/picture"},
        {"http://purl.oclc.org/ooxml/drawingml/chart", "http://schemas.openxmlformats.org/drawingml/2006/chart"},
        {"http://purl.oclc.org/ooxml/drawingml/diagram", "http://schemas.openxmlformats.org/drawingml/2006/diagram"},
        {"http://purl.oclc.org/ooxml/officeDocument/math", "http://schemas.openxmlformats.org/officeDocument/2006/math"},
        {"http://purl.oclc.org/ooxml/officeDocument/sharedTypes", "http://schemas.openxmlformats.org/officeDocument/2006/sharedTypes"},
        {"http://purl.oclc.org/ooxml/officeDocument/extendedProperties", "http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"},
        {"http://purl.oclc.org/ooxml/officeDocument/customProperties", "http://schemas.openxmlformats.org/officeDocument/2006/custom-properties"},
        {"http://purl.oclc.org/ooxml/officeDocument/docPropsVTypes", "http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"},
        {"http://purl.oclc.org/ooxml/officeDocument/customXml", "http://schemas.openxmlformats.org/officeDocument/2006/customXml"},
        {"http://purl.oclc.org/ooxml/schemaLibrary/main", "http://schemas.openxmlformats.org/schemaLibrary/2006/main"},
    };
    return m;
}

constexpr const char* MC = "http://schemas.openxmlformats.org/markup-compatibility/2006";

struct Scope {
    std::map<std::string, std::string> map;  // prefix -> uri
};

std::string canon_uri(std::string u) {
    auto& sm = strict_map();
    if (auto it = sm.find(u); it != sm.end()) return it->second;
    return u;
}

struct Normalizer {
    std::map<std::string, std::string> uri_to_prefix;  // chosen prefix per uri
    std::set<std::string> used_prefixes;

    std::string prefix_for(const std::string& uri, const std::string& hint) {
        if (auto it = uri_to_prefix.find(uri); it != uri_to_prefix.end()) return it->second;
        std::string p(schema::prefix_of_uri(uri));
        if (uri == MC) p = "mc";
        if (p.empty() || used_prefixes.count(p)) {
            p = hint.empty() ? "ns" : hint;
            std::string base = p;
            for (int i = 1; used_prefixes.count(p) || !schema::uri_of_prefix(p).empty(); i++)
                p = base + std::to_string(i);
        }
        uri_to_prefix[uri] = p;
        used_prefixes.insert(p);
        return p;
    }

    std::string rename(const std::string& q, const Scope& s, bool is_attr) {
        auto c = q.find(':');
        std::string pre = c == std::string::npos ? "" : q.substr(0, c);
        std::string loc = c == std::string::npos ? q : q.substr(c + 1);
        if (pre == "xml") return q;
        if (pre.empty() && is_attr) return q;  // unqualified attributes have no namespace
        auto it = s.map.find(pre);
        if (it == s.map.end()) return q;  // undeclared; leave alone
        if (it->second.empty()) return loc;
        return prefix_for(it->second, pre) + ":" + loc;
    }

    void walk(pugi::xml_node n, Scope s, bool root) {
        // read declarations
        std::vector<pugi::xml_attribute> decls;
        for (auto a : n.attributes()) {
            std::string an = a.name();
            if (an == "xmlns") { s.map[""] = canon_uri(a.value()); decls.push_back(a); }
            else if (util::starts_with(an, "xmlns:")) { s.map[an.substr(6)] = canon_uri(a.value()); decls.push_back(a); }
        }
        for (auto& [p, u] : s.map) if (!u.empty()) prefix_for(u, p);
        for (auto& d : decls) n.remove_attribute(d);
        n.set_name(rename(n.name(), s, false).c_str());
        for (auto a : n.attributes()) {
            std::string an = a.name();
            std::string nn = rename(an, s, true);
            if (nn != an) a.set_name(nn.c_str());
            // mc:Ignorable / ProcessContent / MustUnderstand hold prefix lists
            if (nn == "mc:Ignorable" || nn == "mc:MustUnderstand" || nn == "mc:ProcessContent" ||
                nn == "mc:PreserveElements" || nn == "mc:PreserveAttributes") {
                std::string out;
                for (auto tok : util::split(util::trim(a.value()), ' ')) {
                    if (tok.empty()) continue;
                    std::string t = tok, rest;
                    if (auto c = t.find(':'); c != std::string::npos) { rest = t.substr(c); t = t.substr(0, c); }
                    auto it = s.map.find(t);
                    std::string np = it == s.map.end() ? t : prefix_for(it->second, t);
                    out += (out.empty() ? "" : " ") + np + rest;
                }
                a.set_value(out.c_str());
            }
            if (nn == "mc:Requires") {
                std::string out;
                for (auto tok : util::split(util::trim(a.value()), ' ')) {
                    if (tok.empty()) continue;
                    auto it = s.map.find(tok);
                    out += (out.empty() ? "" : " ") + (it == s.map.end() ? tok : prefix_for(it->second, tok));
                }
                a.set_value(out.c_str());
            }
        }
        for (auto c : n.children())
            if (c.type() == pugi::node_element) walk(c, s, false);
    }
};

void collect_prefixes(pugi::xml_node n, std::set<std::string>& out) {
    auto take = [&](std::string_view q) {
        auto c = q.find(':');
        if (c != std::string_view::npos) out.insert(std::string(q.substr(0, c)));
    };
    take(n.name());
    for (auto a : n.attributes()) {
        std::string_view an = a.name();
        if (an == "xmlns" || util::starts_with(an, "xmlns:")) continue;
        take(an);
        if (an == "mc:Ignorable" || an == "mc:ProcessContent" || an == "mc:MustUnderstand") {
            for (auto& t : util::split(util::trim(a.value()), ' ')) if (!t.empty()) take(t + ":");
        }
        if (an == "mc:Requires")
            for (auto& t : util::split(util::trim(a.value()), ' ')) if (!t.empty()) out.insert(t);
    }
    for (auto c : n.children()) if (c.type() == pugi::node_element) collect_prefixes(c, out);
}

}  // namespace

void normalize(pugi::xml_document& doc) {
    auto root = doc.document_element();
    if (!root) return;
    Normalizer nz;
    // Seed: keep the prefixes the document already uses for its known URIs.
    nz.walk(root, Scope{{{"xml", "http://www.w3.org/XML/1998/namespace"}}}, true);
    // Re-declare everything on the root, in a stable order.
    std::vector<std::pair<std::string, std::string>> decls;
    for (auto& [uri, p] : nz.uri_to_prefix) {
        if (p == "xml") continue;
        decls.emplace_back(p, uri);
    }
    std::set<std::string> used;
    collect_prefixes(root, used);
    for (auto& [p, uri] : decls) {
        if (!used.count(p)) continue;
        std::string an = "xmlns:" + p;
        if (!root.attribute(an.c_str())) root.prepend_attribute(an.c_str()) = uri.c_str();
    }
    fix_declarations(root);
}

void declare(pugi::xml_node root, std::string_view prefix, bool ignorable) {
    std::string an = "xmlns:" + std::string(prefix);
    if (!root.attribute(an.c_str())) {
        std::string_view uri = prefix == "mc" ? std::string_view(MC) : schema::uri_of_prefix(prefix);
        if (uri.empty()) return;
        // keep declarations before other attributes
        pugi::xml_attribute last_decl;
        for (auto a : root.attributes())
            if (util::starts_with(a.name(), "xmlns")) last_decl = a;
        auto na = last_decl ? root.insert_attribute_after(an.c_str(), last_decl) : root.prepend_attribute(an.c_str());
        na = std::string(uri).c_str();
    }
    if (ignorable) {
        declare(root, "mc");
        auto ig = root.attribute("mc:Ignorable");
        std::string cur = ig ? ig.value() : "";
        for (auto& t : util::split(cur, ' ')) if (t == prefix) return;
        cur += (cur.empty() ? "" : " ") + std::string(prefix);
        if (!ig) ig = root.append_attribute("mc:Ignorable");
        ig = cur.c_str();
    }
}

void fix_declarations(pugi::xml_node root) {
    std::set<std::string> used;
    collect_prefixes(root, used);
    for (auto& p : used) {
        if (p == "xml" || p == "xmlns") continue;
        std::string an = "xmlns:" + p;
        if (root.attribute(an.c_str())) continue;
        declare(root, p);
    }
    if (auto ig = root.attribute("mc:Ignorable")) {
        std::string out;
        for (auto& t : util::split(util::trim(ig.value()), ' ')) {
            if (t.empty()) continue;
            if (!root.attribute(("xmlns:" + t).c_str())) continue;
            out += (out.empty() ? "" : " ") + t;
        }
        if (out.empty()) root.remove_attribute(ig);
        else ig = out.c_str();
    }
}

std::vector<pugi::xml_node> children(pugi::xml_node n, std::string_view q) {
    std::vector<pugi::xml_node> v;
    std::string s(q);
    for (auto c = n.child(s.c_str()); c; c = c.next_sibling(s.c_str())) v.push_back(c);
    return v;
}

namespace {
void desc(pugi::xml_node n, const std::string& q, std::vector<pugi::xml_node>& v) {
    for (auto c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        if (q == c.name()) v.push_back(c);
        desc(c, q, v);
    }
}
}  // namespace

std::vector<pugi::xml_node> descendants(pugi::xml_node n, std::string_view q) {
    std::vector<pugi::xml_node> v;
    desc(n, std::string(q), v);
    return v;
}

pugi::xml_node first_descendant(pugi::xml_node n, std::string_view q) {
    std::string s(q);
    return n.find_node([&](pugi::xml_node x) { return x.type() == pugi::node_element && s == x.name(); });
}

pugi::xml_node ancestor(pugi::xml_node n, std::string_view q) {
    for (auto p = n.parent(); p; p = p.parent())
        if (p.type() == pugi::node_element && q == p.name()) return p;
    return {};
}

bool is(pugi::xml_node n, std::string_view q) { return n && n.type() == pugi::node_element && q == n.name(); }

pugi::xml_node root_of(pugi::xml_node n) {
    auto r = n.root();
    for (auto c : r.children())
        if (c.type() == pugi::node_element) return c;
    return {};
}

std::string attr(pugi::xml_node n, std::string_view q, std::string_view def) {
    auto a = n.attribute(std::string(q).c_str());
    return a ? std::string(a.value()) : std::string(def);
}

void set_attr(pugi::xml_node n, std::string_view q, std::string_view v) {
    std::string s(q);
    auto a = n.attribute(s.c_str());
    if (!a) a = n.append_attribute(s.c_str());
    a.set_value(std::string(v).c_str());
}

void remove_attr(pugi::xml_node n, std::string_view q) { n.remove_attribute(std::string(q).c_str()); }

namespace {
struct SW : pugi::xml_writer {
    std::string s;
    void write(const void* d, std::size_t n) override { s.append((const char*)d, n); }
};
}  // namespace

std::string to_string(pugi::xml_node n, bool pretty) {
    SW w;
    n.print(w, "  ", pretty ? pugi::format_indent : pugi::format_raw, pugi::encoding_utf8);
    return w.s;
}

bool parse_fragment(std::string_view frag, pugi::xml_document& holder, std::string* error) {
    // Wrap with every canonical namespace so bare prefixes resolve.
    static const char* prefixes[] = {"w", "r", "a", "pic", "wp", "wp14", "w14", "w15", "w16cid", "w16cex", "w16du",
                                     "w16se", "w16", "m", "mc", "v", "o", "w10", "wps", "wpg", "wpc", "a14", "wne", "c", "dgm", "sl", "cx", "aink", "am3d", "w16sdtdh", "w16sdtfl"};
    std::string wrap = "<docx-fragment";
    for (auto p : prefixes) {
        std::string_view uri = std::string_view(p) == "mc" ? std::string_view(MC) : schema::uri_of_prefix(p);
        if (uri.empty()) continue;
        wrap += " xmlns:" + std::string(p) + "=\"" + std::string(uri) + "\"";
    }
    wrap += ">" + std::string(frag) + "</docx-fragment>";
    pugi::xml_document tmp;
    auto r = tmp.load_buffer(wrap.data(), wrap.size(), pugi::parse_default | pugi::parse_ws_pcdata_single);
    if (!r) {
        if (error) *error = r.description();
        return false;
    }
    normalize(tmp);
    holder.reset();
    for (auto c : tmp.document_element().children()) holder.append_copy(c);
    return true;
}

pugi::xml_node copy_into(pugi::xml_node parent, pugi::xml_node src, pugi::xml_node before) {
    if (before) return parent.insert_copy_before(src, before);
    return parent.append_copy(src);
}

}  // namespace docx::xml
