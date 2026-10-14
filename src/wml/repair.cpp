#include "wml/repair.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util/util.hpp"
#include "wml/document.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool name_is(pugi::xml_node n, std::string_view q) { return n && n.type() == pugi::node_element && q == n.name(); }

std::string upper(std::string s) {
    for (auto& c : s) c = char(std::toupper((unsigned char)c));
    return s;
}

bool is_hex6(std::string_view s) {
    if (s.size() != 6) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

// HTML/VML color names older writers put where Word wants RRGGBB.
const std::map<std::string, std::string>& color_names() {
    static const std::map<std::string, std::string> m = {
        {"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"}, {"green", "008000"}, {"lime", "00FF00"},
        {"blue", "0000FF"}, {"yellow", "FFFF00"}, {"cyan", "00FFFF"}, {"aqua", "00FFFF"}, {"magenta", "FF00FF"},
        {"fuchsia", "FF00FF"}, {"orange", "FFA500"}, {"purple", "800080"}, {"gray", "808080"}, {"grey", "808080"},
        {"silver", "C0C0C0"}, {"maroon", "800000"}, {"navy", "000080"}, {"olive", "808000"}, {"teal", "008080"},
        {"darkred", "8B0000"}, {"darkblue", "00008B"}, {"darkgreen", "006400"}, {"darkcyan", "008B8B"},
        {"darkmagenta", "8B008B"}, {"darkgray", "A9A9A9"}, {"darkgrey", "A9A9A9"}, {"lightgray", "D3D3D3"},
        {"lightgrey", "D3D3D3"}, {"lightblue", "ADD8E6"}, {"lightgreen", "90EE90"}, {"pink", "FFC0CB"},
        {"brown", "A52A2A"}, {"gold", "FFD700"}, {"violet", "EE82EE"}, {"indigo", "4B0082"}};
    return m;
}

// ST_HexColor attributes in WML: w:color/w:val (CT_Color), and the color /
// fill attributes of borders and shading.
bool fix_color_attr(pugi::xml_node e, const char* attr) {
    auto a = e.attribute(attr);
    if (!a) return false;
    std::string v = a.value();
    if (v == "auto" || is_hex6(v)) return false;
    std::string s = v;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (is_hex6(s)) { a.set_value(upper(s).c_str()); return true; }
    if (s.size() == 3 && is_hex6(std::string{s[0], s[0], s[1], s[1], s[2], s[2]})) {
        a.set_value(upper(std::string{s[0], s[0], s[1], s[1], s[2], s[2]}).c_str());
        return true;
    }
    auto it = color_names().find(util::lower(s));
    a.set_value(it != color_names().end() ? it->second.c_str() : "auto");
    return true;
}

bool is_border(std::string_view q) {
    static const std::vector<std::string_view> b = {"w:top", "w:left", "w:bottom", "w:right", "w:start", "w:end",
                                                    "w:insideH", "w:insideV", "w:tl2br", "w:tr2bl", "w:between",
                                                    "w:bar", "w:bdr"};
    for (auto x : b)
        if (q == x) return true;
    return false;
}

// Old math revision form: <m:r><w:ins|w:del>(w:rPr, m:t)</w:ins></m:r>.
// The schema form wraps the run instead: <w:ins><m:r>...</m:r></w:ins>.
bool fix_math_run(pugi::xml_node mr) {
    pugi::xml_node rev;
    for (auto c : mr.children())
        if (name_is(c, "w:ins") || name_is(c, "w:del")) { rev = c; break; }
    if (!rev) return false;
    // only the old form carries run content inside the marker
    bool content = false;
    for (auto c : rev.children())
        if (c.type() == pugi::node_element) content = true;
    auto parent = mr.parent();
    auto wrap = parent.insert_child_before(rev.name(), mr);
    for (auto a : rev.attributes()) wrap.append_attribute(a.name()) = a.value();
    if (content) {
        for (auto c = rev.first_child(); c;) {
            auto nx = c.next_sibling();
            mr.insert_move_before(c, rev);
            c = nx;
        }
    }
    mr.remove_child(rev);
    wrap.append_move(mr);
    return true;
}

// Unwrap an element, keeping its children in place (minus `drop` children).
void unwrap(pugi::xml_node n, std::string_view drop) {
    auto parent = n.parent();
    for (auto c = n.first_child(); c;) {
        auto nx = c.next_sibling();
        if (!name_is(c, drop)) parent.insert_move_before(c, n);
        c = nx;
    }
    parent.remove_child(n);
}

}  // namespace

int repair_part(Document& d, pugi::xml_node root) {
    int n = 0;
    std::vector<pugi::xml_node> smart, math_runs, tables;
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
        auto e = stack.back();
        stack.pop_back();
        for (auto c = e.last_child(); c; c = c.previous_sibling())
            if (c.type() == pugi::node_element) stack.push_back(c);
        std::string_view q = e.name();
        if (q == "w:color") {
            n += fix_color_attr(e, "w:val");
        } else if (q == "w:shd") {
            n += fix_color_attr(e, "w:color");
            n += fix_color_attr(e, "w:fill");
        } else if (is_border(q) && e.attribute("w:val")) {
            n += fix_color_attr(e, "w:color");
        } else if (q == "w:w" && name_is(e.parent(), "w:rPr")) {
            // character scale 0 is invalid; Word reads it as 100%
            auto v = std::string(e.attribute("w:val").value());
            if (v == "0" || v == "0%") { e.parent().remove_child(e); n++; }
        } else if (q == "w:smartTag") {
            smart.push_back(e);
        } else if (q == "w:smartTagType" && name_is(e.parent(), "w:settings")) {
            smart.push_back(e);
        } else if (q == "m:r") {
            math_runs.push_back(e);
        } else if (q == "w:moveFromRangeStart" || q == "w:moveToRangeStart") {
            if (!e.attribute("w:date")) { xml::set_attr(e, "w:date", d.date()); n++; }
            if (!e.attribute("w:author")) { xml::set_attr(e, "w:author", d.author.name); n++; }
        } else if (q == "w:tbl") {
            tables.push_back(e);
        }
    }
    // Smart tags were removed from the Office schema (Word 2010 drops them on
    // save, keeping the text). Unwrap deepest first.
    for (auto it = smart.rbegin(); it != smart.rend(); ++it) {
        if (name_is(*it, "w:smartTagType")) it->parent().remove_child(*it);
        else unwrap(*it, "w:smartTagPr");
        n++;
    }
    for (auto mr : math_runs)
        if (mr.parent()) n += fix_math_run(mr);
    // A table needs tblPr and tblGrid (accepting a tracked table insert in
    // some writers leaves them out).
    for (auto t : tables) {
        if (!t.child("w:tblPr")) {
            auto pr = t.prepend_child("w:tblPr");
            auto w = pr.append_child("w:tblW");
            xml::set_attr(w, "w:w", "0");
            xml::set_attr(w, "w:type", "auto");
            n++;
        }
        if (!t.child("w:tblGrid")) {
            int cols = 0;
            for (auto tr : t.children("w:tr")) {
                int c = 0;
                for (auto tc : tr.children("w:tc")) {
                    auto span = tc.child("w:tcPr").child("w:gridSpan");
                    c += span ? std::max(1, span.attribute("w:val").as_int(1)) : 1;
                }
                cols = std::max(cols, c);
            }
            auto g = t.insert_child_after("w:tblGrid", t.child("w:tblPr"));
            for (int i = 0; i < std::max(1, cols); i++) xml::set_attr(g.append_child("w:gridCol"), "w:w", "2000");
            n++;
        }
    }
    return n;
}

}  // namespace docx::wml
