// Fields, table of contents, content controls, lists, equations.
#include <map>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/fields_internal.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool tracking(Document& d, const EditOpts& o) { return o.track ? *o.track : d.track_changes(); }

// Container + insert-before node for visible pos (like place_run in content.cpp).
std::pair<pugi::xml_node, pugi::xml_node> insert_spot(pugi::xml_node p, int pos) {
    split_at(p, pos);
    ParaText pt = analyze(p);
    int n = (int)pt.vis.size();
    if (pos < n) {
        auto node = pt.atoms[pt.vis[pos]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        return {node.parent(), node};
    }
    if (n > 0) {
        auto node = pt.atoms[pt.vis[n - 1]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        return {node.parent(), past_field_result(node)};
    }
    return {p, {}};
}

pugi::xml_node add_run(pugi::xml_node parent, pugi::xml_node before) {
    return before ? parent.insert_child_before("w:r", before) : parent.append_child("w:r");
}

std::string roman(int n, bool upper) {
    static const std::pair<int, const char*> t[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                    {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    std::string s;
    for (auto& [v, r] : t) while (n >= v) { s += r; n -= v; }
    if (upper) for (auto& c : s) c = char(std::toupper((unsigned char)c));
    return s;
}

std::string letters(int n, bool upper) {
    // Word repeats the letter: a..z, aa..zz, aaa..
    if (n < 1) return "";
    int rep = (n - 1) / 26 + 1;
    char c = char('a' + (n - 1) % 26);
    if (upper) c = char(std::toupper((unsigned char)c));
    return std::string(rep, c);
}

std::string ordinal_suffix(int n) {
    int m = n % 100;
    if (m >= 11 && m <= 13) return "th";
    switch (n % 10) { case 1: return "st"; case 2: return "nd"; case 3: return "rd"; default: return "th"; }
}

std::string cardinal(int n) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                 "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (n < 0) return "minus " + cardinal(-n);
    if (n < 20) return ones[n];
    if (n < 100) return std::string(tens[n / 10]) + (n % 10 ? "-" + std::string(ones[n % 10]) : "");
    if (n < 1000) return std::string(ones[n / 100]) + " hundred" + (n % 100 ? " " + cardinal(n % 100) : "");
    if (n < 1000000) return cardinal(n / 1000) + " thousand" + (n % 1000 ? " " + cardinal(n % 1000) : "");
    return cardinal(n / 1000000) + " million" + (n % 1000000 ? " " + cardinal(n % 1000000) : "");
}

}  // namespace

std::string detail::number_words(int n) { return cardinal(n); }

std::string format_number(int n, const std::string& fmt) {
    if (fmt == "decimal" || fmt.empty()) return std::to_string(n);
    if (fmt == "decimalZero") return (n < 10 ? "0" : "") + std::to_string(n);
    if (fmt == "lowerRoman") return roman(n, false);
    if (fmt == "upperRoman") return roman(n, true);
    if (fmt == "lowerLetter") return letters(n, false);
    if (fmt == "upperLetter") return letters(n, true);
    if (fmt == "ordinal") return std::to_string(n) + ordinal_suffix(n);
    if (fmt == "cardinalText") { auto s = cardinal(n); s[0] = char(std::toupper((unsigned char)s[0])); return s; }
    if (fmt == "ordinalText") {
        static const char* o[] = {"Zeroth", "First", "Second", "Third", "Fourth", "Fifth", "Sixth", "Seventh", "Eighth", "Ninth", "Tenth"};
        return n <= 10 ? o[n] : std::to_string(n) + ordinal_suffix(n);
    }
    if (fmt == "decimalEnclosedParen") return "(" + std::to_string(n) + ")";
    if (fmt == "hex") { char b[16]; std::snprintf(b, sizeof b, "%X", n); return b; }
    if (fmt == "bullet" || fmt == "none") return "";
    return std::to_string(n);
}

// ── fields ───────────────────────────────────────────────────────────────

void insert_field(Document& d, pugi::xml_node p, int pos, const std::string& instr, const std::string& result,
                  const Json& rpr, const EditOpts& o) {
    if (util::trim(instr).empty()) throw std::runtime_error("field instruction is empty");
    auto [parent, before] = insert_spot(p, pos);
    pugi::xml_node wrap_parent = parent, wrap_before = before;
    if (tracking(d, o)) {
        auto ins = before ? parent.insert_child_before("w:ins", before) : parent.append_child("w:ins");
        stamp(d, ins);
        wrap_parent = ins;
        wrap_before = {};
    }
    auto mk = [&](auto fill) {
        auto r = add_run(wrap_parent, wrap_before);
        if (rpr.is_object() && !rpr.empty()) apply_rpr_json(d, r.append_child("w:rPr"), rpr);
        fill(r);
        return r;
    };
    auto chr = [&](const char* type) { mk([&](pugi::xml_node r) { xml::set_attr(r.append_child("w:fldChar"), "w:fldCharType", type); }); };
    auto code = [&](const std::string& s) {
        if (s.empty()) return;
        mk([&](pugi::xml_node r) {
            auto t = r.append_child("w:instrText");
            xml::set_attr(t, "xml:space", "preserve");
            t.text().set(s.c_str());
        });
    };
    // { } in the instruction are nested fields (Word's Ctrl+F9 braces);
    // braces inside quotes stay text unless they open a field
    std::string s = " " + util::trim(instr) + " ";
    std::string buf;
    int depth = 0;
    chr("begin");
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '{' || s[i + 1] == '}')) { buf += s[++i]; continue; }
        if (c == '{') {
            code(buf);
            buf.clear();
            chr("begin");
            depth++;
            continue;
        }
        if (c == '}' && depth > 0) {
            code(buf);
            buf.clear();
            chr("separate");
            chr("end");
            depth--;
            continue;
        }
        buf += c;
    }
    code(buf);
    while (depth-- > 0) { chr("separate"); chr("end"); }
    chr("separate");
    if (!result.empty()) mk([&](pugi::xml_node r) { append_text(r, util::to_u32(result)); });
    chr("end");
}

// ── index (References > Mark Entry / Insert Index) ───────────────────────────────────────────

namespace detail {
void set_update_fields(Document& d);
}
using detail::set_update_fields;

namespace {
std::string xe_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == ':') o += "\\:";
        else o += c;
    }
    return o;
}

// "main:sub" terms of every XE field (escaped colons come back as \x01).
std::vector<std::string> xe_terms(Document& d) {
    std::vector<std::string> v;
    for (auto p : paragraphs(d.body(), true)) {
        std::string instr;
        bool in = false;
        std::vector<pugi::xml_node> seq;
        std::function<void(pugi::xml_node)> walk = [&](pugi::xml_node n) {
            for (auto c : n.children()) {
                std::string_view q = c.name();
                if (q == "w:fldChar" || q == "w:instrText") seq.push_back(c);
                else if (c.type() == pugi::node_element && q != "mc:Fallback") walk(c);
            }
        };
        walk(p);
        for (auto e : seq) {
            if (std::string_view(e.name()) == "w:fldChar") {
                std::string t = xml::attr(e, "w:fldCharType");
                if (t == "begin") { in = true; instr.clear(); continue; }
                if (!in) continue;
                in = false;  // instruction ends at separate (or end when there's none)
                std::string s = util::trim(instr);
                if (!util::starts_with(s, "XE ")) continue;
                auto a = s.find('"'), b = s.rfind('"');
                if (a == std::string::npos || b <= a) continue;
                std::string term;
                for (std::size_t i = a + 1; i < b; i++) {
                    if (s[i] == '\\' && i + 1 < b) { ++i; term += s[i] == ':' ? '\x01' : s[i]; continue; }
                    term += s[i];
                }
                v.push_back(term);
            } else if (in) {
                instr += e.text().get();
            }
        }
    }
    return v;
}

// The section properties in force at `n` (its section's sectPr).
pugi::xml_node section_at(Document& d, pugi::xml_node n) {
    for (auto c = n; c; c = c.next_sibling())
        if (auto s = c.child("w:pPr").child("w:sectPr")) return s;
    return d.body().child("w:sectPr");
}

// A copy of `like`'s page setup as a continuous section break with `cols` columns.
void continuous_break(pugi::xml_node p, pugi::xml_node like, int cols) {
    auto sect = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:sectPr");
    for (auto q : {"w:pgSz", "w:pgMar", "w:docGrid"})
        if (auto e = like.child(q)) sect.append_copy(e);
    xml::set_attr(sect.append_child("w:type"), "w:val", "continuous");
    auto c = sect.append_child("w:cols");
    xml::set_attr(c, "w:space", "720");
    if (cols > 1) xml::set_attr(c, "w:num", std::to_string(cols));
    schema::sort_children(sect);
}
}  // namespace

void mark_index_entry(Document& d, pugi::xml_node p, int pos, const std::string& term, const std::string& sub,
                      const EditOpts& o) {
    if (util::trim(term).empty()) throw std::runtime_error("index entry text is empty");
    std::string instr = "XE \"" + xe_escape(term) + (sub.empty() ? "" : ":" + xe_escape(sub)) + "\"";
    insert_field(d, p, pos, instr, "", nullptr, o);
}

int insert_index(Document& d, pugi::xml_node before, const Json& opts_in, const EditOpts& o) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    int cols = opts.value("columns", 2);
    if (cols < 1 || cols > 4) throw std::runtime_error("columns must be 1-4");
    // main term -> sub terms, sorted the way Word sorts an index
    auto ci_less = [](const std::string& a, const std::string& b) { return util::lower(a) < util::lower(b); };
    std::map<std::string, std::set<std::string, decltype(ci_less)>, decltype(ci_less)> terms(ci_less);
    for (auto t : xe_terms(d)) {
        auto c = t.find(':');
        std::string main = t.substr(0, c), sub = c == std::string::npos ? "" : t.substr(c + 1);
        for (auto* s : {&main, &sub}) for (auto& ch : *s) if (ch == '\x01') ch = ':';
        main = util::trim(main);
        if (main.empty()) continue;
        auto it = terms.try_emplace(main, ci_less).first;
        if (!util::trim(sub).empty()) it->second.insert(std::string(util::trim(sub)));
    }
    std::string instr = "INDEX \\c \"" + std::to_string(cols) + "\" \\z \"1033\"";
    if (before.parent() != d.body())
        throw std::runtime_error("the index goes between body paragraphs, not in a table, text box or header");
    auto parent = before.parent();
    pugi::xml_node like = section_at(d, before);
    std::vector<pugi::xml_node> made;
    auto new_p = [&](const std::string& style) {
        auto np = parent.insert_child_before("w:p", before);
        np.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        np.append_attribute("w14:textId") = "77777777";
        if (!style.empty()) xml::set_attr(np.append_child("w:pPr").append_child("w:pStyle"), "w:val", ensure_style(d, style));
        made.push_back(np);
        return np;
    };
    auto fld = [&](pugi::xml_node p, const char* type) { xml::set_attr(p.append_child("w:r").append_child("w:fldChar"), "w:fldCharType", type); };
    auto code = [&](pugi::xml_node p) {
        fld(p, "begin");
        auto it = p.append_child("w:r").append_child("w:instrText");
        xml::set_attr(it, "xml:space", "preserve");
        it.text().set((" " + instr + " ").c_str());
        fld(p, "separate");
    };
    // the text before keeps its own section settings: end that section here
    {
        auto bp = new_p("");
        auto ppr = schema::ensure_child(bp, "w:pPr");
        auto sect = ppr.append_copy(like);
        // header/footer references stay with the final section; drop the copies
        for (auto q : {"w:headerReference", "w:footerReference"})
            while (sect.child(q)) sect.remove_child(q);
        sect.remove_child("w:sectPrChange");
        sect.remove_attribute("w:rsidR");
        sect.remove_attribute("w:rsidSect");
        // the index section starts on the same page
        xml::set_attr(schema::ensure_child(sect, "w:type"), "w:val", "continuous");
    }
    if (terms.empty()) {
        auto p = new_p("");
        code(p);
        append_text(p.append_child("w:r"), U"No index entries found.");
        fld(p, "end");
    } else {
        std::string letter;
        bool opened = false;
        for (auto& [main, subs] : terms) {
            std::string l(1, char(std::toupper((unsigned char)main[0])));
            if (l != letter) {
                letter = l;
                auto hp = new_p("IndexHeading");
                if (!opened) { code(hp); opened = true; }
                append_text(hp.append_child("w:r"), util::to_u32(letter));
            }
            // page numbers need layout: update_fields with layout fills them in, Word does on open
            append_text(new_p("Index1").append_child("w:r"), util::to_u32(main + (subs.empty() ? ", 1" : "")));
            for (auto& s : subs) append_text(new_p("Index2").append_child("w:r"), util::to_u32(s + ", 1"));
        }
        fld(made.back(), "end");
    }
    // the index's own section ends on its last paragraph: continuous, in columns
    continuous_break(made.back(), like, cols);
    if (tracking(d, o))
        for (auto mp : made) mark_paragraph_inserted(d, mp);
    set_update_fields(d);
    xml::declare(xml::root_of(before), "w14", true);
    int n = 0;
    for (auto& [m, s] : terms) n += 1 + (int)s.size();
    return n;
}

int detail::rebuild_indexes(Document& d, const std::map<std::string, std::vector<std::string>>* pages) {
    // Each index is a run of IndexHeading / Index1 / Index2 paragraphs; the
    // field code sits in the first, the end mark in the last. Rewrite the
    // entry text in place: "term, 3, 7".
    int n = 0;
    auto body = d.body();
    // every (main, sub) pair the XE fields name
    std::map<std::pair<std::string, std::string>, int> terms;
    for (auto t : xe_terms(d)) {
        auto c = t.find(':');
        std::replace(t.begin(), t.end(), '\x01', ':');
        if (c == std::string::npos) terms[{t, ""}] = 1;
        else { terms[{t.substr(0, c), ""}] = 1; terms[{t.substr(0, c), t.substr(c + 1)}] = 1; }
    }
    auto* known = &terms;
    auto kids = blocks(body);
    for (std::size_t i = 0; i < kids.size(); i++) {
        auto p = kids[i];
        bool has_index = false;
        for (auto it : xml::descendants(p, "w:instrText"))
            if (util::starts_with(util::trim(it.text().get()), "INDEX")) has_index = true;
        if (!has_index) continue;
        std::string main;
        for (std::size_t k = i; k < kids.size(); k++) {
            auto q = kids[k];
            std::string st = xml::attr(q.child("w:pPr").child("w:pStyle"), "w:val");
            if (k > i && st != "IndexHeading" && st != "Index1" && st != "Index2") break;
            if (st != "Index1" && st != "Index2") continue;
            // the term is the longest known XE term the text starts with
            auto pt = analyze(q);
            std::string t = pt.utf8();
            std::string term;
            for (auto& [k2, v2] : *known) {
                std::string cand = st == "Index1" ? k2.first : k2.second;
                if (st == "Index2" && !util::iequals(k2.first, main)) continue;
                if (cand.empty() || cand.size() <= term.size()) continue;
                if (util::iequals(t.substr(0, cand.size()), cand) && (t.size() == cand.size() || t.compare(cand.size(), 2, ", ") == 0))
                    term = t.substr(0, cand.size());
            }
            if (term.empty()) term = t;
            std::string key = st == "Index1" ? util::lower(term) : util::lower(main) + "\x02" + util::lower(term);
            if (st == "Index1") main = term;
            if (!pages) continue;  // nothing new to say
            std::string label;
            if (auto f = pages->find(key); f != pages->end())
                for (auto& pg : f->second) label += (label.empty() ? "" : ", ") + pg;
            std::string want = term + (label.empty() ? "" : ", " + label);
            if (want == t) continue;
            // rewrite the visible text, keeping field chars in place
            for (auto r : xml::children(q, "w:r")) {
                if (r.child("w:fldChar") || r.child("w:instrText")) continue;
                q.remove_child(r);
            }
            pugi::xml_node endrun;
            for (auto r : xml::children(q, "w:r"))
                if (auto fc = r.child("w:fldChar"); fc && xml::attr(fc, "w:fldCharType") == "end") endrun = r;
            auto nr = endrun ? q.insert_child_before("w:r", endrun) : q.append_child("w:r");
            append_text(nr, util::to_u32(want));
            n++;
        }
    }
    return n;
}

std::string detail::xe_term(const std::string& code) {
    std::string s = util::trim(code);
    if (!util::starts_with(s, "XE ")) return "";
    auto a = s.find('"'), b = s.rfind('"');
    if (a == std::string::npos || b <= a) return "";
    std::string term;
    for (std::size_t i = a + 1; i < b; i++) {
        if (s[i] == '\\' && i + 1 < b) { ++i; term += s[i] == ':' ? '\x01' : s[i]; continue; }
        term += s[i];
    }
    return term;
}

std::vector<FieldInfo> list_fields(Document& d) {
    std::vector<FieldInfo> v;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto fs : xml::descendants(root, "w:fldSimple")) {
            FieldInfo f;
            f.instr = util::trim(xml::attr(fs, "w:instr"));
            for (auto t : xml::descendants(fs, "w:t")) f.result += t.text().get();
            if (auto p = xml::ancestor(fs, "w:p")) f.para = xml::attr(p, "w14:paraId");
            v.push_back(f);
        }
        // complex fields: walk in document order
        struct Open { FieldInfo f; bool code = true; };
        std::vector<Open> stack;
        std::function<void(pugi::xml_node, pugi::xml_node)> walk = [&](pugi::xml_node n, pugi::xml_node para) {
            for (auto k : n.children()) {
                if (k.type() != pugi::node_element) continue;
                std::string_view q = k.name();
                if (q == "w:p") { walk(k, k); continue; }
                if (q == "mc:Fallback" || q == "w:del" || q == "w:moveFrom") continue;
                if (q == "w:fldChar") {
                    std::string t = xml::attr(k, "w:fldCharType");
                    if (t == "begin") { stack.push_back({}); stack.back().f.para = xml::attr(para, "w14:paraId"); }
                    else if (t == "separate" && !stack.empty()) stack.back().code = false;
                    else if (t == "end" && !stack.empty()) {
                        auto f = stack.back().f;
                        f.instr = util::trim(f.instr);
                        stack.pop_back();
                        v.push_back(f);
                        if (!stack.empty() && !stack.back().code) stack.back().f.result += f.result;
                    }
                    continue;
                }
                if (q == "w:instrText" && !stack.empty() && stack.back().code) { stack.back().f.instr += k.text().get(); continue; }
                if (q == "w:t" && !stack.empty() && !stack.back().code) { stack.back().f.result += k.text().get(); continue; }
                if (q == "w:tab" && !stack.empty() && !stack.back().code) { stack.back().f.result += "\t"; continue; }
                walk(k, para);
            }
        };
        walk(root, {});
    }
    return v;
}

namespace {

struct Heading { pugi::xml_node p; int level; std::string text; };

int heading_level(Document& d, pugi::xml_node p) {
    // outline level from direct pPr or the style chain (Heading 1..9 carry outlineLvl)
    std::function<int(pugi::xml_node)> from = [&](pugi::xml_node ppr) -> int {
        if (auto o = ppr.child("w:outlineLvl")) {
            int v = util::to_int(xml::attr(o, "w:val", "9"));
            return v < 9 ? v + 1 : 0;
        }
        return -1;
    };
    int v = from(p.child("w:pPr"));
    if (v >= 0) return v;
    std::string sid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
    std::set<std::string> seen;
    while (!sid.empty() && !seen.count(sid)) {
        seen.insert(sid);
        auto s = find_style(d, sid);
        if (!s) break;
        int lv = from(s.child("w:pPr"));
        if (lv >= 0) return lv;
        sid = xml::attr(s.child("w:basedOn"), "w:val");
    }
    return 0;
}

std::vector<Heading> headings(Document& d, int lo, int hi) {
    std::vector<Heading> v;
    for (auto p : paragraphs(d.body(), false)) {
        if (xml::ancestor(p, "w:sdt") && xml::first_descendant(xml::ancestor(p, "w:sdt").child("w:sdtPr"), "w:docPartGallery")) {
            std::string g = xml::attr(xml::first_descendant(xml::ancestor(p, "w:sdt").child("w:sdtPr"), "w:docPartGallery"), "w:val");
            if (g == "Table of Contents") continue;
        }
        int lv = heading_level(d, p);
        if (lv < lo || lv > hi || lv == 0) continue;
        std::string t = util::trim(analyze(p).utf8());
        if (t.empty()) continue;
        v.push_back({p, lv, t});
    }
    return v;
}

// Bookmark name Word gives a heading for TOC links (_Toc + 9 digits).
std::string toc_bookmark(Document& d, pugi::xml_node p) {
    for (auto b : xml::children(p, "w:bookmarkStart")) {
        std::string n = xml::attr(b, "w:name");
        if (util::starts_with(n, "_Toc")) return n;
    }
    std::string name;
    std::set<std::string> have;
    for (auto& b : list_bookmarks(d)) have.insert(b.name);
    do { name = "_Toc" + std::to_string(100000000 + util::random_u32() % 900000000); } while (have.count(name));
    std::string id = std::to_string(d.next_bookmark_id());
    auto ppr = p.child("w:pPr");
    auto bs = ppr ? p.insert_child_after("w:bookmarkStart", ppr) : p.prepend_child("w:bookmarkStart");
    xml::set_attr(bs, "w:id", id);
    xml::set_attr(bs, "w:name", name);
    auto be = p.append_child("w:bookmarkEnd");
    xml::set_attr(be, "w:id", id);
    return name;
}

long right_tab(Document& d) {
    auto sp = d.body().child("w:sectPr");
    long w = util::to_long(xml::attr(sp.child("w:pgSz"), "w:w", "12240"));
    long l = util::to_long(xml::attr(sp.child("w:pgMar"), "w:left", "1440"));
    long r = util::to_long(xml::attr(sp.child("w:pgMar"), "w:right", "1440"));
    return w - l - r;
}

struct TocEntry { pugi::xml_node p; int level; std::string text; };

// Caption paragraphs whose number is a SEQ field for `label` (Table of Figures).
std::vector<TocEntry> caption_entries(Document& d, const std::string& label) {
    std::vector<TocEntry> v;
    for (auto p : paragraphs(d.body(), false)) {
        bool has = false;
        for (auto it : xml::descendants(p, "w:instrText")) {
            std::string s = util::trim(it.text().get());
            if (util::starts_with(s, "SEQ ")) {
                std::string id = util::trim(s.substr(4));
                id = id.substr(0, id.find(' '));
                if (util::iequals(id, label)) has = true;
            }
        }
        for (auto fs : xml::descendants(p, "w:fldSimple")) {
            std::string s = util::trim(xml::attr(fs, "w:instr"));
            if (util::starts_with(s, "SEQ ") && util::iequals(util::trim(s.substr(4)).substr(0, label.size()), label)) has = true;
        }
        if (has) v.push_back({p, 1, analyze(p).utf8()});
    }
    return v;
}

// What a TOC field lists: headings (\o "1-3") or captions (\c "Figure").
std::vector<TocEntry> toc_entries(Document& d, const std::string& instr) {
    auto c = instr.find("\\c \"");
    if (c != std::string::npos) {
        auto e = instr.find('"', c + 4);
        return caption_entries(d, instr.substr(c + 4, e == std::string::npos ? std::string::npos : e - c - 4));
    }
    int lo = 1, hi = 3;
    auto q = instr.find("\\o \"");
    if (q != std::string::npos) std::sscanf(instr.c_str() + q + 4, "%d-%d", &lo, &hi);
    std::vector<TocEntry> v;
    for (auto& h : headings(d, lo, hi)) v.push_back({h.p, h.level, h.text});
    return v;
}

// Build the TOC entry paragraphs (between the field's separate and end).
void build_toc_entries(Document& d, pugi::xml_node sdt_content, bool links, const std::string& instr) {
    auto hs = toc_entries(d, instr);
    bool figures = instr.find("\\c \"") != std::string::npos;
    long tab = right_tab(d);
    std::vector<pugi::xml_node> made;
    for (std::size_t i = 0; i < hs.size(); i++) {
        auto& h = hs[i];
        std::string bm = toc_bookmark(d, h.p);
        std::string sty = ensure_style(d, figures ? "TableofFigures" : "TOC" + std::to_string(h.level));
        auto p = sdt_content.append_child("w:p");
        p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.append_attribute("w14:textId") = "77777777";
        auto ppr = p.append_child("w:pPr");
        xml::set_attr(ppr.append_child("w:pStyle"), "w:val", sty);
        auto tabs = ppr.append_child("w:tabs");
        auto t = tabs.append_child("w:tab");
        xml::set_attr(t, "w:val", "right");
        xml::set_attr(t, "w:leader", "dot");
        xml::set_attr(t, "w:pos", std::to_string(tab));
        ppr.append_child("w:rPr").append_child("w:noProof");
        if (i == 0) {
            auto r1 = p.append_child("w:r");
            xml::set_attr(r1.append_child("w:fldChar"), "w:fldCharType", "begin");
            auto r2 = p.append_child("w:r");
            auto it = r2.append_child("w:instrText");
            xml::set_attr(it, "xml:space", "preserve");
            it.text().set((" " + instr + " ").c_str());
            auto r3 = p.append_child("w:r");
            xml::set_attr(r3.append_child("w:fldChar"), "w:fldCharType", "separate");
        }
        pugi::xml_node holder = p;
        if (links) {
            holder = p.append_child("w:hyperlink");
            xml::set_attr(holder, "w:anchor", bm);
            xml::set_attr(holder, "w:history", "1");
        }
        auto tr = holder.append_child("w:r");
        auto trp = tr.append_child("w:rPr");
        if (links) xml::set_attr(trp.append_child("w:rStyle"), "w:val", ensure_style(d, "Hyperlink"));
        trp.append_child("w:noProof");
        append_text(tr, util::to_u32(h.text));
        auto webr = [&](auto fill) {
            auto r = holder.append_child("w:r");
            auto rp = r.append_child("w:rPr");
            rp.append_child("w:noProof");
            rp.append_child("w:webHidden");
            fill(r);
        };
        webr([](pugi::xml_node r) { r.append_child("w:tab"); });
        webr([](pugi::xml_node r) { xml::set_attr(r.append_child("w:fldChar"), "w:fldCharType", "begin"); });
        webr([&](pugi::xml_node r) {
            auto x = r.append_child("w:instrText");
            xml::set_attr(x, "xml:space", "preserve");
            x.text().set((" PAGEREF " + bm + " \\h ").c_str());
        });
        webr([](pugi::xml_node r) { xml::set_attr(r.append_child("w:fldChar"), "w:fldCharType", "separate"); });
        // page numbers need layout; Word refreshes them on open (updateFields)
        webr([](pugi::xml_node r) { append_text(r, U"1"); });
        webr([](pugi::xml_node r) { xml::set_attr(r.append_child("w:fldChar"), "w:fldCharType", "end"); });
        made.push_back(p);
    }
    // closing paragraph with the field end
    auto pe = sdt_content.append_child("w:p");
    pe.append_attribute("w14:paraId") = d.new_hex_id().c_str();
    pe.append_attribute("w14:textId") = "77777777";
    if (hs.empty()) {
        // empty TOC: Word shows its "No table of contents entries found." result
        auto r1 = pe.append_child("w:r");
        xml::set_attr(r1.append_child("w:fldChar"), "w:fldCharType", "begin");
        auto r2 = pe.append_child("w:r");
        auto it = r2.append_child("w:instrText");
        xml::set_attr(it, "xml:space", "preserve");
        it.text().set((" " + instr + " ").c_str());
        auto r3 = pe.append_child("w:r");
        xml::set_attr(r3.append_child("w:fldChar"), "w:fldCharType", "separate");
        auto r4 = pe.append_child("w:r");
        r4.append_child("w:rPr").append_child("w:noProof");
        append_text(r4, figures ? U"No table of figures entries found." : U"No table of contents entries found.");
    }
    auto re = pe.append_child("w:r");
    auto rp = re.append_child("w:rPr");
    rp.append_child("w:b");
    rp.append_child("w:bCs");
    rp.append_child("w:noProof");
    xml::set_attr(re.append_child("w:fldChar"), "w:fldCharType", "end");
}

}  // namespace

namespace detail {
void set_update_fields(Document& d) {
    auto st = d.settings();
    auto u = schema::ensure_child(st, "w:updateFields");
    xml::set_attr(u, "w:val", "true");
}
int outline_level(Document& d, pugi::xml_node p) { return heading_level(d, p); }
}  // namespace detail

void insert_toc(Document& d, pugi::xml_node p, const Json& opts_in, const EditOpts& o) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    int lo = opts.value("from_level", 1), hi = opts.value("to_level", 3);
    bool links = opts.value("hyperlinks", true);
    std::string captions = opts.value("captions", std::string());
    std::string title = opts.value("title", captions.empty() ? std::string("Contents") : std::string());
    std::string instr = captions.empty()
        ? "TOC \\o \"" + std::to_string(lo) + "-" + std::to_string(hi) + "\"" + (links ? " \\h" : "") + " \\z \\u"
        : "TOC " + std::string(links ? "\\h " : "") + "\\z \\c \"" + captions + "\"";
    auto sdt = p.parent().insert_child_before("w:sdt", p);
    auto pr = sdt.append_child("w:sdtPr");
    xml::set_attr(pr.append_child("w:id"), "w:val", std::to_string((std::int32_t)util::random_u32()));
    auto dpo = pr.append_child("w:docPartObj");
    xml::set_attr(dpo.append_child("w:docPartGallery"), "w:val", "Table of Contents");
    dpo.append_child("w:docPartUnique");
    auto epr = sdt.append_child("w:sdtEndPr").append_child("w:rPr");
    epr.append_child("w:b");
    epr.append_child("w:bCs");
    epr.append_child("w:noProof");
    auto content = sdt.append_child("w:sdtContent");
    if (!title.empty()) {
        auto tp = content.append_child("w:p");
        tp.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        tp.append_attribute("w14:textId") = "77777777";
        xml::set_attr(tp.append_child("w:pPr").append_child("w:pStyle"), "w:val", ensure_style(d, "TOCHeading"));
        append_text(tp.append_child("w:r"), util::to_u32(title));
    }
    build_toc_entries(d, content, links, instr);
    if (tracking(d, o)) {
        for (auto tp : xml::descendants(content, "w:p")) mark_paragraph_inserted(d, tp);
    }
    set_update_fields(d);
    xml::declare(xml::root_of(p), "w14", true);
}

int detail::rebuild_tocs(Document& d) {
    int n = 0;
    for (auto sdt : xml::descendants(d.body(), "w:sdt")) {
        auto g = xml::first_descendant(sdt.child("w:sdtPr"), "w:docPartGallery");
        if (!g || xml::attr(g, "w:val") != "Table of Contents") continue;
        auto content = sdt.child("w:sdtContent");
        std::string instr;
        for (auto it : xml::descendants(content, "w:instrText")) {
            std::string s = util::trim(it.text().get());
            if (util::starts_with(s, "TOC")) { instr = s; break; }
        }
        if (instr.empty()) continue;
        int lo = 1, hi = 3;
        auto q = instr.find("\\o \"");
        if (q != std::string::npos) std::sscanf(instr.c_str() + q + 4, "%d-%d", &lo, &hi);
        bool links = instr.find("\\h") != std::string::npos;
        // keep the heading paragraph (first non-entry paragraph with TOCHeading style)
        std::vector<pugi::xml_node> drop;
        for (auto c : content.children()) {
            if (std::string_view(c.name()) == "w:p" && xml::attr(c.child("w:pPr").child("w:pStyle"), "w:val") == "TOCHeading") continue;
            drop.push_back(c);
        }
        for (auto c : drop) content.remove_child(c);
        build_toc_entries(d, content, links, instr);
        n++;
    }
    return n;
}

int update_fields(Document& d) { return update_fields(d, FieldUpdate{}).updated; }

// ── captions and cross-references ──────────────────────────────────────────────────

namespace {
// SEQ identifier of a caption paragraph, or "".
std::string caption_label(pugi::xml_node p) {
    for (auto it : xml::descendants(p, "w:instrText")) {
        auto w = util::split(util::trim(it.text().get()), ' ');
        if (w.size() > 1 && util::iequals(w[0], "SEQ")) return w[1];
    }
    for (auto f : xml::descendants(p, "w:fldSimple")) {
        auto w = util::split(util::trim(xml::attr(f, "w:instr")), ' ');
        if (w.size() > 1 && util::iequals(w[0], "SEQ")) return w[1];
    }
    return "";
}

// The caption label of a target given as a paragraph id, or "".
std::string lbl_of_target(Document& d, const std::string& target) {
    try {
        return caption_label(find_para(d, target).p);
    } catch (...) {
        return "";
    }
}

// A hidden _Ref bookmark over visible [from, to) of p: an existing one with
// exactly that text is reused (Word adds one per distinct range).
std::string ref_bookmark(Document& d, pugi::xml_node p, int from, int to) {
    auto t = analyze(p).text;
    std::string want = util::to_utf8(t.substr(from, to - from));
    std::set<std::string> taken;
    for (auto& b : list_bookmarks(d)) {
        taken.insert(b.name);
        if (util::starts_with(b.name, "_Ref") && b.para == para_id(p) && b.text == want) return b.name;
    }
    std::string name;
    do name = "_Ref" + std::to_string(100000000 + util::random_u32() % 900000000);
    while (taken.count(name));
    add_bookmark(d, p, from, p, to, name);
    return name;
}
}  // namespace

pugi::xml_node insert_caption(Document& d, pugi::xml_node ref, bool after, const std::string& label_in,
                              const std::string& text, const std::string& separator, const EditOpts& o) {
    std::string label = util::trim(label_in.empty() ? "Figure" : label_in);
    for (char c : label)
        if (!(std::isalnum((unsigned char)c) || c == '_'))
            throw std::runtime_error("a caption label is one word (Figure, Table, Equation, or your own)");
    // Word puts table captions above the table by default and others below; the caller decides
    auto p = insert_paragraph(d, ref, after, U"", "Caption", nullptr, o);
    insert_text(d, p, 0, util::to_u32(label + " "), nullptr, o);
    insert_field(d, p, (int)analyze(p).vis.size(), "SEQ " + label + " \\* ARABIC", "1", nullptr, o);
    std::string sep = separator.empty() ? ": " : separator;
    if (!text.empty()) insert_text(d, p, (int)analyze(p).vis.size(), util::to_u32(sep + text), nullptr, o);
    update_fields(d);
    return p;
}

std::vector<RefTarget> reference_targets(Document& d) {
    std::vector<RefTarget> v;
    for (auto p : paragraphs(d.body(), false)) {
        std::string t = util::trim(analyze(p).utf8());
        if (t.empty()) continue;
        if (int lv = heading_level(d, p); lv >= 1 && lv <= 9) {
            v.push_back({"heading", para_id(p), t, "", para_id(p)});
            continue;
        }
        if (auto lbl = caption_label(p); !lbl.empty()) v.push_back({"caption", para_id(p), t, lbl, para_id(p)});
    }
    for (auto& b : list_bookmarks(d))
        if (!util::starts_with(b.name, "_")) v.push_back({"bookmark", b.name, b.text, "", b.para});
    for (auto& n : list_notes(d)) v.push_back({n.kind, n.kind + ":" + n.id, n.text, "", n.anchor_para});
    return v;
}

std::string insert_cross_reference(Document& d, pugi::xml_node p, int pos, const std::string& target,
                                   const std::string& show_in, bool link, const EditOpts& o) {
    std::string show = util::lower(show_in.empty() ? "text" : show_in);
    std::string h = link ? " \\h" : "";
    std::string instr;
    // notes: NOTEREF to a bookmark around the reference mark
    if (util::starts_with(target, "footnote:") || util::starts_with(target, "endnote:")) {
        bool fn = target[0] == 'f';
        std::string id = target.substr(target.find(':') + 1);
        pugi::xml_node mark;
        for (auto r : xml::descendants(d.body(), fn ? "w:footnoteReference" : "w:endnoteReference"))
            if (xml::attr(r, "w:id") == id) mark = r;
        if (!mark) throw std::runtime_error("no " + std::string(fn ? "footnote" : "endnote") + " " + id + " (list_notes shows them)");
        auto mp = xml::ancestor(mark, "w:p");
        // the mark's visible offset
        ParaText pt = analyze(mp);
        int at = 0;
        for (std::size_t i = 0; i < pt.vis.size(); i++)
            if (pt.atoms[pt.vis[i]].run == mark.parent()) at = (int)i;
        std::string bm;
        for (auto& b : list_bookmarks(d))
            if (util::starts_with(b.name, "_Ref") && b.para == para_id(mp) && b.text.size() <= 4) bm = b.name;
        if (bm.empty()) bm = ref_bookmark(d, mp, at, at + 1);
        instr = "NOTEREF " + bm + h;
        if (show == "page") instr = "PAGEREF " + bm + h;
        else if (show == "above_below") instr = "NOTEREF " + bm + " \\p" + h;
    } else {
        // a bookmark by name, or a heading / caption paragraph by id
        std::string bm;
        for (auto& b : list_bookmarks(d))
            if (b.name == target) bm = b.name;
        if (bm.empty()) {
            auto r = find_para(d, target);
            if (r.part != d.main()) throw std::runtime_error("cross-references point at body paragraphs");
            int n = (int)analyze(r.p).vis.size();
            auto lbl = caption_label(r.p);
            int from = 0, to = n;
            if (!lbl.empty() && (show == "text" || show == "label_number" || show == "number")) {
                // Word splits a caption at its separator: "Figure 1" | ": " | "text"
                auto t = analyze(r.p).text;
                auto k = t.find_first_of(U":.-\u2013\u2014");
                if (k != std::u32string::npos) {
                    if (show == "text") {
                        from = (int)k + 1;
                        while (from < n && t[from] == U' ') from++;
                    } else {
                        to = (int)k;
                        while (to > 0 && t[to - 1] == U' ') to--;
                        if (show == "number") {
                            // just the number: back over the digits before the separator
                            from = to;
                            while (from > 0 && t[from - 1] >= U'0' && t[from - 1] <= U'9') from--;
                        }
                    }
                }
            }
            bm = ref_bookmark(d, r.p, from, to);
        }
        bool caption_num = show == "number" && !lbl_of_target(d, target).empty();
        if (show == "text" || show == "label_number" || caption_num) instr = "REF " + bm + h;
        else if (show == "number") instr = "REF " + bm + " \\r" + h;
        else if (show == "page") instr = "PAGEREF " + bm + h;
        else if (show == "above_below") instr = "REF " + bm + " \\p" + h;
        else throw std::runtime_error(util::unknown_key("show", show, {"text", "number", "label_number", "page", "above_below"}));
    }
    // PAGEREF needs layout; Word refreshes it on open (updateFields), until
    // then show a page number rather than nothing
    insert_field(d, p, pos, instr, util::starts_with(instr, "PAGEREF") ? "1" : "", nullptr, o);
    update_fields(d);
    return instr;
}

// ── content controls ─────────────────────────────────────────────────────

pugi::xml_node add_content_control(Document& d, pugi::xml_node p, int s, int e, const Json& opts_in) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    std::string type = opts.value("type", std::string("richText"));
    auto runs = isolate(p, s, e);
    auto top = [&](pugi::xml_node x) { while (std::string_view(x.parent().name()) != "w:p") x = x.parent(); return x; };
    pugi::xml_node before;
    if (!runs.empty()) before = top(runs.front());
    else {
        auto spot = insert_spot(p, s);
        before = spot.second;
    }
    auto sdt = before ? p.insert_child_before("w:sdt", before) : p.append_child("w:sdt");
    auto pr = sdt.append_child("w:sdtPr");
    if (opts.contains("alias")) xml::set_attr(pr.append_child("w:alias"), "w:val", opts["alias"].get<std::string>());
    if (opts.contains("tag")) xml::set_attr(pr.append_child("w:tag"), "w:val", opts["tag"].get<std::string>());
    xml::set_attr(pr.append_child("w:id"), "w:val", std::to_string((std::int32_t)util::random_u32()));
    if (opts.value("lock_delete", false) || opts.value("lock_content", false)) {
        std::string lk = opts.value("lock_delete", false) && opts.value("lock_content", false) ? "sdtContentLocked"
                       : opts.value("lock_delete", false) ? "sdtLocked" : "contentLocked";
        xml::set_attr(schema::ensure_child(pr, "w:lock"), "w:val", lk);
    }
    bool placeholder = runs.empty();
    if (placeholder) {
        xml::set_attr(schema::ensure_child(schema::ensure_child(pr, "w:placeholder"), "w:docPart"), "w:val", "DefaultPlaceholder_-1854013440");
        schema::ensure_child(pr, "w:showingPlcHdr");
    }
    if (type == "text") schema::ensure_child(pr, "w:text");
    else if (type == "checkbox") {
        auto cb = pr.append_child("w14:checkbox");
        bool on = opts.value("checked", false);
        xml::set_attr(cb.append_child("w14:checked"), "w14:val", on ? "1" : "0");
        auto cs = cb.append_child("w14:checkedState");
        xml::set_attr(cs, "w14:val", "2612");
        xml::set_attr(cs, "w14:font", "MS Gothic");
        auto us = cb.append_child("w14:uncheckedState");
        xml::set_attr(us, "w14:val", "2610");
        xml::set_attr(us, "w14:font", "MS Gothic");
        xml::declare(xml::root_of(p), "w14", true);
    } else if (type == "dropDown" || type == "comboBox") {
        auto dd = pr.append_child(type == "dropDown" ? "w:dropDownList" : "w:comboBox");
        for (auto& it : opts.value("items", Json::array())) {
            auto li = dd.append_child("w:listItem");
            std::string disp = it.is_string() ? it.get<std::string>() : it.value("text", std::string());
            std::string val = it.is_string() ? it.get<std::string>() : it.value("value", disp);
            xml::set_attr(li, "w:displayText", disp);
            xml::set_attr(li, "w:value", val);
        }
    } else if (type == "date") {
        auto dt = pr.append_child("w:date");
        xml::set_attr(dt.append_child("w:dateFormat"), "w:val", opts.value("format", std::string("M/d/yyyy")));
        xml::set_attr(dt.append_child("w:lid"), "w:val", "en-US");
        xml::set_attr(dt.append_child("w:storeMappedDataAs"), "w:val", "dateTime");
        xml::set_attr(dt.append_child("w:calendar"), "w:val", "gregorian");
    } else if (type == "picture") pr.append_child("w:picture");
    else if (type != "richText") throw std::runtime_error("unknown content control type " + type);
    // keep schema order inside sdtPr
    schema::sort_children(pr);
    auto content = sdt.append_child("w:sdtContent");
    if (placeholder) {
        std::string ph = opts.value("placeholder", std::string(type == "checkbox" ? "" : "Click or tap here to enter text."));
        auto r = content.append_child("w:r");
        if (type == "checkbox") {
            r.append_child("w:rPr").append_child("w:rFonts");
            auto f = r.child("w:rPr").child("w:rFonts");
            xml::set_attr(f, "w:ascii", "MS Gothic");
            xml::set_attr(f, "w:eastAsia", "MS Gothic");
            xml::set_attr(f, "w:hAnsi", "MS Gothic");
            pr.remove_child("w:showingPlcHdr");
            append_text(r, opts.value("checked", false) ? U"\u2612" : U"\u2610");
        } else {
            xml::set_attr(r.append_child("w:rPr").append_child("w:rStyle"), "w:val", ensure_style(d, "PlaceholderText"));
            append_text(r, util::to_u32(ph));
        }
    } else {
        std::vector<pugi::xml_node> mv;
        for (auto n = top(runs.front()); n; n = n.next_sibling()) {
            mv.push_back(n);
            if (n == top(runs.back())) break;
        }
        for (auto n : mv) content.append_move(n);
    }
    return sdt;
}

std::vector<ControlInfo> list_content_controls(Document& d) {
    std::vector<ControlInfo> v;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        for (auto sdt : xml::descendants(d.pkg().xml(part).document_element(), "w:sdt")) {
            auto pr = sdt.child("w:sdtPr");
            ControlInfo c;
            c.id = xml::attr(pr.child("w:id"), "w:val");
            c.tag = xml::attr(pr.child("w:tag"), "w:val");
            c.alias = xml::attr(pr.child("w:alias"), "w:val");
            c.type = pr.child("w:text") ? "text" : pr.child("w14:checkbox") ? "checkbox" : pr.child("w:dropDownList") ? "dropDown"
                   : pr.child("w:comboBox") ? "comboBox" : pr.child("w:date") ? "date" : pr.child("w:picture") ? "picture"
                   : pr.child("w:docPartObj") ? "docPart" : pr.child("w:richText") ? "richText" : pr.child("w:group") ? "group"
                   : pr.child("w15:repeatingSection") ? "repeatingSection" : "richText";
            if (auto cb = pr.child("w14:checkbox")) c.checked = xml::attr(cb.child("w14:checked"), "w14:val") == "1";
            for (auto t : xml::descendants(sdt.child("w:sdtContent"), "w:t")) c.text += t.text().get();
            if (pr.child("w:showingPlcHdr")) c.text = "";
            v.push_back(c);
        }
    }
    return v;
}

void set_content_control(Document& d, const std::string& key, const Json& value) {
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        for (auto sdt : xml::descendants(d.pkg().xml(part).document_element(), "w:sdt")) {
            auto pr = sdt.child("w:sdtPr");
            if (xml::attr(pr.child("w:id"), "w:val") != key && xml::attr(pr.child("w:tag"), "w:val") != key &&
                xml::attr(pr.child("w:alias"), "w:val") != key)
                continue;
            auto content = sdt.child("w:sdtContent");
            if (auto cb = pr.child("w14:checkbox")) {
                bool on = value.is_boolean() ? value.get<bool>() : value.is_string() && value.get<std::string>() == "true";
                xml::set_attr(cb.child("w14:checked"), "w14:val", on ? "1" : "0");
                for (auto t : xml::descendants(content, "w:t")) { t.text().set(on ? "\u2612" : "\u2610"); break; }
                return;
            }
            std::string text = value.is_string() ? value.get<std::string>() : value.dump();
            if (auto dd = pr.child("w:dropDownList")) {
                bool ok = false;
                for (auto li : dd.children("w:listItem"))
                    if (xml::attr(li, "w:displayText") == text || xml::attr(li, "w:value") == text) { text = xml::attr(li, "w:displayText"); ok = true; }
                if (!ok) throw std::runtime_error("'" + text + "' is not one of the drop-down items");
            }
            if (auto dt = pr.child("w:date")) xml::set_attr(dt, "w:fullDate", text.size() == 10 ? text + "T00:00:00Z" : text);
            pr.remove_child("w:showingPlcHdr");
            // replace content text, keeping the first run's formatting (minus placeholder style)
            bool block = std::string_view(sdt.parent().name()) != "w:p";
            pugi::xml_node fmt;
            if (auto r = xml::first_descendant(content, "w:r")) fmt = r.child("w:rPr");
            pugi::xml_document keep;
            if (fmt) { auto c = keep.append_copy(fmt); c.remove_child("w:rStyle"); }
            if (block) {
                auto p0 = content.child("w:p");
                pugi::xml_document kp;
                if (p0 && p0.child("w:pPr")) kp.append_copy(p0.child("w:pPr"));
                std::vector<pugi::xml_node> all;
                for (auto c : content.children()) all.push_back(c);
                for (auto c : all) content.remove_child(c);
                for (auto& line : util::split(text, '\n')) {
                    auto p = content.append_child("w:p");
                    if (kp.first_child()) p.append_copy(kp.first_child());
                    auto r = p.append_child("w:r");
                    if (keep.first_child() && keep.first_child().first_child()) r.append_copy(keep.first_child());
                    append_text(r, util::to_u32(line));
                }
            } else {
                std::vector<pugi::xml_node> all;
                for (auto c : content.children()) all.push_back(c);
                for (auto c : all) content.remove_child(c);
                auto r = content.append_child("w:r");
                if (keep.first_child() && keep.first_child().first_child()) r.append_copy(keep.first_child());
                append_text(r, util::to_u32(text));
            }
            return;
        }
    }
    throw std::runtime_error("content control '" + key + "' not found (use id, tag or title)");
}

// ── lists ────────────────────────────────────────────────────────────────

namespace {

const char* kBulletChars[3] = {"\xEF\x82\xB7", "o", "\xEF\x82\xA7"};  // U+F0B7 (Symbol), o, U+F0A7 (Wingdings)
const char* kBulletFonts[3] = {"Symbol", "Courier New", "Wingdings"};

std::string hex8() {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", util::random_u32());
    return b;
}

// Word's default list definitions (Home > Bullets / Numbering).
pugi::xml_node add_abstract(Document& d, const std::string& kind, const Json& opts) {
    auto num = d.numbering();
    int aid = 0;
    for (auto a : num.children("w:abstractNum")) aid = std::max(aid, a.attribute("w:abstractNumId").as_int() + 1);
    // abstractNums must precede nums
    auto a = schema::insert_child(num, "w:abstractNum");
    xml::set_attr(a, "w:abstractNumId", std::to_string(aid));
    xml::set_attr(a, "w15:restartNumberingAfterBreak", "0");
    xml::set_attr(a.append_child("w:nsid"), "w:val", hex8());
    xml::set_attr(a.append_child("w:multiLevelType"), "w:val", "hybridMultilevel");
    xml::set_attr(a.append_child("w:tmpl"), "w:val", hex8());
    std::string fmt0 = kind == "bullet" ? "bullet" : kind == "number" ? "decimal" : kind;
    static const char* numfmts[3] = {"decimal", "lowerLetter", "lowerRoman"};
    static const char* tplc_num[3] = {"0409000F", "04090019", "0409001B"};
    static const char* tplc_bul[3] = {"04090001", "04090003", "04090005"};
    for (int l = 0; l < 9; l++) {
        auto lvl = a.append_child("w:lvl");
        xml::set_attr(lvl, "w:ilvl", std::to_string(l));
        xml::set_attr(lvl, "w:tplc", fmt0 == "bullet" ? tplc_bul[l % 3] : tplc_num[l % 3]);
        if (l > 0) xml::set_attr(lvl, "w:tentative", "1");
        xml::set_attr(lvl.append_child("w:start"), "w:val", std::to_string(l == 0 ? opts.value("start", 1) : 1));
        std::string fmt = fmt0 == "bullet" ? "bullet" : l == 0 ? fmt0 : numfmts[l % 3];
        xml::set_attr(lvl.append_child("w:numFmt"), "w:val", fmt);
        std::string text = fmt == "bullet" ? kBulletChars[l % 3] : "%" + std::to_string(l + 1) + ".";
        if (l == 0 && opts.contains("text")) text = opts["text"].get<std::string>();
        if (l == 0 && fmt == "bullet" && opts.contains("bullet")) text = opts["bullet"].get<std::string>();
        xml::set_attr(lvl.append_child("w:lvlText"), "w:val", text);
        bool right = fmt == "lowerRoman" || fmt == "upperRoman";
        xml::set_attr(lvl.append_child("w:lvlJc"), "w:val", right ? "right" : "left");
        auto ind = lvl.append_child("w:pPr").append_child("w:ind");
        xml::set_attr(ind, "w:left", std::to_string(720 * (l + 1)));
        xml::set_attr(ind, "w:hanging", right ? "180" : "360");
        if (fmt == "bullet") {
            auto rf = lvl.append_child("w:rPr").append_child("w:rFonts");
            std::string font = (l == 0 && opts.contains("bullet")) ? opts.value("bullet_font", std::string("Symbol")) : kBulletFonts[l % 3];
            xml::set_attr(rf, "w:ascii", font);
            xml::set_attr(rf, "w:hAnsi", font);
            if (font == "Courier New") xml::set_attr(rf, "w:cs", font);
            xml::set_attr(rf, "w:hint", "default");
        }
    }
    xml::declare(xml::root_of(num), "w15", true);
    return a;
}

int add_num(Document& d, int abstract_id, int restart_level = -1, int start = 1) {
    auto num = d.numbering();
    int nid = 1;
    for (auto n : num.children("w:num")) nid = std::max(nid, n.attribute("w:numId").as_int() + 1);
    auto n = schema::insert_child(num, "w:num");
    xml::set_attr(n, "w:numId", std::to_string(nid));
    xml::set_attr(n.append_child("w:abstractNumId"), "w:val", std::to_string(abstract_id));
    if (restart_level >= 0) {
        auto ov = n.append_child("w:lvlOverride");
        xml::set_attr(ov, "w:ilvl", std::to_string(restart_level));
        xml::set_attr(ov.append_child("w:startOverride"), "w:val", std::to_string(start));
    }
    return nid;
}

pugi::xml_node abstract_of(Document& d, int num_id) {
    auto num = d.numbering(false);
    if (!num) return {};
    std::string aid;
    for (auto n : num.children("w:num"))
        if (n.attribute("w:numId").as_int() == num_id) aid = xml::attr(n.child("w:abstractNumId"), "w:val");
    if (aid.empty()) return {};
    for (auto a : num.children("w:abstractNum"))
        if (xml::attr(a, "w:abstractNumId") == aid) return a;
    return {};
}

std::pair<int, int> num_of(Document& d, pugi::xml_node p) {
    auto np = p.child("w:pPr").child("w:numPr");
    if (np) return {util::to_int(xml::attr(np.child("w:numId"), "w:val", "0")), util::to_int(xml::attr(np.child("w:ilvl"), "w:val", "0"))};
    // from the paragraph style
    std::string sid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
    std::set<std::string> seen;
    while (!sid.empty() && !seen.count(sid)) {
        seen.insert(sid);
        auto s = find_style(d, sid);
        if (!s) break;
        if (auto snp = s.child("w:pPr").child("w:numPr"))
            return {util::to_int(xml::attr(snp.child("w:numId"), "w:val", "0")), util::to_int(xml::attr(snp.child("w:ilvl"), "w:val", "0"))};
        sid = xml::attr(s.child("w:basedOn"), "w:val");
    }
    return {0, 0};
}

}  // namespace

int apply_list(Document& d, const std::vector<pugi::xml_node>& ps, const std::string& kind, int level, const Json& opts_in,
               const EditOpts& o) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    if (ps.empty()) throw std::runtime_error("no paragraphs given");
    int num_id = opts.value("num_id", 0);
    if (!num_id) {
        // continue the list of the paragraph just before, like Word does when the kind matches
        if (opts.value("continue_previous", true)) {
            auto prev = ps.front().previous_sibling("w:p");
            if (prev) {
                auto [pn, pl] = num_of(d, prev);
                if (pn) {
                    auto a = abstract_of(d, pn);
                    std::string f = a ? xml::attr(a.child("w:lvl").child("w:numFmt"), "w:val") : "";
                    bool want_bullet = kind == "bullet";
                    if ((f == "bullet") == want_bullet) num_id = pn;
                }
            }
        }
        if (!num_id) {
            auto a = add_abstract(d, kind, opts);
            num_id = add_num(d, a.attribute("w:abstractNumId").as_int());
        }
    }
    std::string lp = ensure_style(d, "ListParagraph");
    for (auto p : ps) {
        Json props = {{"numbering", {{"num_id", num_id}, {"level", level}}}};
        std::string cur = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
        if (cur.empty() || cur == "Normal") props["style"] = lp;
        format_paragraph(d, p, props, o);
        if (tracking(d, o)) {
            // Word records new numbering as w:ins inside numPr
            auto np = p.child("w:pPr").child("w:numPr");
            if (np && !np.child("w:ins")) stamp(d, schema::ensure_child(np, "w:ins"));
        }
    }
    return num_id;
}

void remove_list(Document& d, const std::vector<pugi::xml_node>& ps, const EditOpts& o) {
    for (auto p : ps) {
        Json props = Json::object();
        auto [n, l] = num_of(d, p);
        if (!n) continue;
        if (p.child("w:pPr").child("w:numPr")) props["numbering"] = nullptr;
        if (xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val") == "ListParagraph") props["style"] = nullptr;
        format_paragraph(d, p, props, o);
        // numbering coming from the style: override with numId 0 (Word does)
        if (num_of(d, p).first) {
            auto np = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:numPr");
            xml::set_attr(schema::ensure_child(np, "w:numId"), "w:val", "0");
        }
    }
}

std::string list_label(Document& d, pugi::xml_node p) {
    auto [num_id, ilvl] = num_of(d, p);
    if (!num_id) return "";
    auto a = abstract_of(d, num_id);
    if (!a) return "";
    // count prior paragraphs in the same list (num shares abstract counters per numId)
    std::vector<int> counters(9, 0);
    auto start_of = [&](int lvl) {
        // lvlOverride/startOverride on the num wins
        auto num = d.numbering(false);
        for (auto n : num.children("w:num")) {
            if (n.attribute("w:numId").as_int() != num_id) continue;
            for (auto ov : n.children("w:lvlOverride"))
                if (ov.attribute("w:ilvl").as_int() == lvl)
                    if (auto so = ov.child("w:startOverride")) return so.attribute("w:val").as_int();
        }
        for (auto l : a.children("w:lvl"))
            if (l.attribute("w:ilvl").as_int() == lvl) return util::to_int(xml::attr(l.child("w:start"), "w:val", "1"));
        return 1;
    };
    // Word keeps counters per num instance; a deeper level resets when a
    // shallower one advances.
    for (auto q : paragraphs(d.body(), false)) {
        auto [qn, ql] = num_of(d, q);
        if (qn == num_id) {
            if (counters[ql] == 0) counters[ql] = start_of(ql) - 1;
            counters[ql]++;
            for (int k = ql + 1; k < 9; k++) counters[k] = 0;
        }
        if (q == p) break;
    }
    pugi::xml_node lvl;
    for (auto l : a.children("w:lvl")) if (l.attribute("w:ilvl").as_int() == ilvl) lvl = l;
    if (!lvl) return "";
    std::string fmt = xml::attr(lvl.child("w:numFmt"), "w:val", "decimal");
    std::string text = xml::attr(lvl.child("w:lvlText"), "w:val");
    if (fmt == "bullet") {
        // map private-use symbol chars to their Unicode look
        if (text == "\xEF\x82\xB7") return "\u2022";
        if (text == "\xEF\x82\xA7") return "\u25AA";
        return text;
    }
    std::string out;
    for (std::size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '1' && text[i + 1] <= '9') {
            int k = text[i + 1] - '1';
            std::string kfmt = fmt;
            for (auto l : a.children("w:lvl"))
                if (l.attribute("w:ilvl").as_int() == k) kfmt = xml::attr(l.child("w:numFmt"), "w:val", "decimal");
            if (lvl.child("w:isLgl")) kfmt = "decimal";
            out += format_number(std::max(1, counters[k]), kfmt);
            i++;
        } else {
            out += text[i];
        }
    }
    return out;
}

void restart_numbering(Document& d, pugi::xml_node p, int start) {
    auto [num_id, ilvl] = num_of(d, p);
    if (!num_id) throw std::runtime_error("paragraph is not in a list");
    auto a = abstract_of(d, num_id);
    int nid = add_num(d, a.attribute("w:abstractNumId").as_int(), ilvl, start);
    // this paragraph and the rest of the same list move to the new num;
    // paragraphs outside any list don't end it, another list does
    bool on = false;
    for (auto q : paragraphs(d.body(), false)) {
        if (q == p) on = true;
        if (!on) continue;
        auto [qn, ql] = num_of(d, q);
        if (qn == 0) continue;
        if (qn != num_id) break;
        auto np = schema::ensure_child(schema::ensure_child(q, "w:pPr"), "w:numPr");
        xml::set_attr(schema::ensure_child(np, "w:ilvl"), "w:val", std::to_string(ql));
        xml::set_attr(schema::ensure_child(np, "w:numId"), "w:val", std::to_string(nid));
    }
}

// ── math ─────────────────────────────────────────────────────────────────

namespace {

struct MathParser {
    std::u32string s;
    std::size_t i = 0;

    bool eof() const { return i >= s.size(); }
    char32_t peek() const { return eof() ? 0 : s[i]; }

    static void text_run(pugi::xml_node parent, const std::u32string& t) {
        if (t.empty()) return;
        auto r = parent.append_child("m:r");
        auto rpr = r.append_child("w:rPr").append_child("w:rFonts");
        xml::set_attr(rpr, "w:ascii", "Cambria Math");
        xml::set_attr(rpr, "w:hAnsi", "Cambria Math");
        auto mt = r.append_child("m:t");
        std::string u = util::to_utf8(t);
        if (u.front() == ' ' || u.back() == ' ') xml::set_attr(mt, "xml:space", "preserve");
        mt.text().set(u.c_str());
    }

    static void ctrl(pugi::xml_node pr) {
        auto c = pr.append_child("m:ctrlPr").append_child("w:rPr").append_child("w:rFonts");
        xml::set_attr(c, "w:ascii", "Cambria Math");
        xml::set_attr(c, "w:hAnsi", "Cambria Math");
    }

    // {...} group or a single atom
    void arg(pugi::xml_node into) {
        if (peek() == '{') {
            i++;
            seq(into, '}');
            if (peek() == '}') i++;
        } else {
            atom(into);
        }
    }

    std::u32string command() {
        std::u32string c;
        while (!eof() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) c.push_back(s[i++]);
        return c;
    }

    void atom(pugi::xml_node into) {
        if (eof()) return;
        char32_t c = s[i];
        if (c == '\\') {
            i++;
            auto cmd = util::to_utf8(command());
            static const std::map<std::string, std::string> sym = {
                {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"theta", "θ"},
                {"lambda", "λ"}, {"mu", "μ"}, {"pi", "π"}, {"sigma", "σ"}, {"phi", "φ"}, {"omega", "ω"},
                {"Delta", "Δ"}, {"Sigma", "Σ"}, {"Omega", "Ω"}, {"infty", "∞"}, {"pm", "±"}, {"times", "×"},
                {"cdot", "⋅"}, {"leq", "≤"}, {"geq", "≥"}, {"neq", "≠"}, {"approx", "≈"}, {"to", "→"},
                {"rightarrow", "→"}, {"partial", "∂"}, {"nabla", "∇"}, {"in", "∈"}, {"forall", "∀"}, {"exists", "∃"}};
            if (cmd == "frac") {
                auto f = into.append_child("m:f");
                ctrl(f.append_child("m:fPr"));
                arg(f.append_child("m:num"));
                arg(f.append_child("m:den"));
                return;
            }
            if (cmd == "sqrt") {
                auto r = into.append_child("m:rad");
                auto pr = r.append_child("m:radPr");
                xml::set_attr(pr.append_child("m:degHide"), "m:val", "1");
                ctrl(pr);
                r.append_child("m:deg");
                arg(r.append_child("m:e"));
                return;
            }
            if (cmd == "sum" || cmd == "int" || cmd == "prod") {
                auto n = into.append_child("m:nary");
                auto pr = n.append_child("m:naryPr");
                std::string chr = cmd == "sum" ? "∑" : cmd == "int" ? "∫" : "∏";
                xml::set_attr(pr.append_child("m:chr"), "m:val", chr);
                auto sub = n.append_child("m:sub");
                auto sup = n.append_child("m:sup");
                for (int k = 0; k < 2; k++) {
                    if (peek() == '_') { i++; arg(sub); }
                    else if (peek() == '^') { i++; arg(sup); }
                }
                // CT_NaryPr order: chr, limLoc, grow, subHide, supHide, ctrlPr
                if (!sub.first_child()) xml::set_attr(pr.append_child("m:subHide"), "m:val", "1");
                if (!sup.first_child()) xml::set_attr(pr.append_child("m:supHide"), "m:val", "1");
                ctrl(pr);
                auto e = n.append_child("m:e");
                atom(e);
                return;
            }
            if (auto it = sym.find(cmd); it != sym.end()) { text_run(into, util::to_u32(it->second)); return; }
            text_run(into, util::to_u32(cmd));
            return;
        }
        if (c == '(' ) {
            i++;
            auto dd = into.append_child("m:d");
            ctrl(dd.append_child("m:dPr"));
            seq(dd.append_child("m:e"), ')');
            if (peek() == ')') i++;
            return;
        }
        if (c == '{') { arg(into); return; }
        std::u32string t;
        auto letter = [](char32_t x) { return x >= 128 || std::isalnum((int)x); };
        if (letter(c)) {
            // a run of letters and digits, any script (ħ, é, Greek typed directly)
            while (!eof() && letter(s[i]) && s[i] != 0x2061) t.push_back(s[i++]);
        } else {
            t.push_back(s[i++]);  // one symbol; stray ) } included, so we always move on
        }
        text_run(into, t);
    }

    void seq(pugi::xml_node into, char32_t close) {
        while (!eof() && peek() != close) {
            if (peek() == ' ') { i++; continue; }
            std::size_t at = i;
            // base followed by ^ or _
            pugi::xml_document tmp;
            auto holder = tmp.append_child("h");
            atom(holder);
            bool sub = false, sup = false;
            pugi::xml_document subd, supd;
            auto subn = subd.append_child("m:sub"), supn = supd.append_child("m:sup");
            for (int k = 0; k < 2; k++) {
                if (peek() == '_') { i++; arg(subn); sub = true; }
                else if (peek() == '^') { i++; arg(supn); sup = true; }
            }
            if (!sub && !sup) {
                for (auto c : holder.children()) into.append_copy(c);
                if (i == at) text_run(into, std::u32string(1, s[i++]));  // never stall
                continue;
            }
            const char* q = sub && sup ? "m:sSubSup" : sub ? "m:sSub" : "m:sSup";
            auto e = into.append_child(q);
            ctrl(e.append_child((std::string(q) + "Pr").c_str()));
            auto base = e.append_child("m:e");
            for (auto c : holder.children()) base.append_copy(c);
            if (sub) e.append_copy(subn);
            if (sup) e.append_copy(supn);
        }
    }
};

}  // namespace

pugi::xml_node insert_equation(Document& d, pugi::xml_node p, int pos, const std::string& linear, bool display, const EditOpts& o) {
    MathParser mp;
    mp.s = util::to_u32(linear);
    pugi::xml_document tmp;
    auto math = tmp.append_child("m:oMath");
    mp.seq(math, 0);
    auto [parent, before] = insert_spot(p, pos);
    pugi::xml_node out;
    if (display) {
        auto para = before ? parent.insert_child_before("m:oMathPara", before) : parent.append_child("m:oMathPara");
        out = para.append_copy(math);
    } else {
        out = before ? parent.insert_copy_before(math, before) : parent.append_copy(math);
    }
    if (tracking(d, o))
        for (auto r : xml::descendants(out, "m:r")) wrap_math_run(d, r, "w:ins");
    xml::declare(xml::root_of(p), "m");
    return out;
}

}  // namespace docx::wml
