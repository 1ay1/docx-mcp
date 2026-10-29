// HTML to rich blocks for insert_html. A forgiving tag-soup reader: what a
// browser clipboard, an email body or a generator produces. Unknown tags pass
// their text through; script/style/head content is dropped.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "util/util.hpp"
#include "wml/markdown.hpp"

namespace docx::wml {

namespace {

struct Node {
    std::string tag;  // lower case; "" = text
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<Node> kids;
};

std::string decode_entities(const std::string& s) {
    static const std::map<std::string, std::string> named = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", "\xC2\xA0"}, {"ndash", "\xE2\x80\x93"},
        {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"}, {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
        {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}, {"bull", "\xE2\x80\xA2"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
        {"trade", "\xE2\x84\xA2"}, {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"}, {"yen", "\xC2\xA5"}, {"cent", "\xC2\xA2"},
        {"sect", "\xC2\xA7"}, {"para", "\xC2\xB6"}, {"deg", "\xC2\xB0"}, {"plusmn", "\xC2\xB1"}, {"times", "\xC3\x97"},
        {"divide", "\xC3\xB7"}, {"frac12", "\xC2\xBD"}, {"frac14", "\xC2\xBC"}, {"frac34", "\xC2\xBE"}, {"middot", "\xC2\xB7"},
        {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"}, {"rarr", "\xE2\x86\x92"}, {"larr", "\xE2\x86\x90"}, {"shy", ""},
        {"zwj", "\xE2\x80\x8D"}, {"zwnj", "\xE2\x80\x8C"}, {"thinsp", "\xE2\x80\x89"}, {"ensp", "\xE2\x80\x82"}, {"emsp", "\xE2\x80\x83"}};
    std::string o;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) { o += '&'; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (!e.empty() && e[0] == '#') {
            long cp = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtol(e.c_str() + 2, nullptr, 16) : std::strtol(e.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 0x110000 && !(cp >= 0xD800 && cp <= 0xDFFF)) o += util::to_utf8((char32_t)cp);
            i = semi;
            continue;
        }
        auto it = named.find(e);
        if (it == named.end()) { o += '&'; continue; }
        o += it->second;
        i = semi;
    }
    return o;
}

const std::set<std::string>& void_tags() {
    static const std::set<std::string> s = {"br", "hr", "img", "meta", "link", "input", "col", "area", "base", "wbr", "source", "embed", "param", "track"};
    return s;
}

// Tags whose start implicitly closes an open one of these (p inside p, li inside li, ...).
bool closes(const std::string& open, const std::string& incoming) {
    static const std::set<std::string> blocks = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "table", "pre", "blockquote", "hr", "section", "article", "header", "footer"};
    if (open == "p" && blocks.count(incoming)) return true;
    if (open == "li" && incoming == "li") return true;
    if ((open == "td" || open == "th") && (incoming == "td" || incoming == "th" || incoming == "tr")) return true;
    if (open == "tr" && incoming == "tr") return true;
    if ((open == "dt" || open == "dd") && (incoming == "dt" || incoming == "dd")) return true;
    if (open == "option" && incoming == "option") return true;
    return false;
}

Node parse_tree(const std::string& s) {
    Node root;
    root.tag = "#root";
    std::vector<Node*> stack{&root};
    std::size_t i = 0;
    auto add_text = [&](const std::string& t) {
        if (t.empty()) return;
        Node n;
        n.text = decode_entities(t);
        stack.back()->kids.push_back(std::move(n));
    };
    while (i < s.size()) {
        auto lt = s.find('<', i);
        if (lt == std::string::npos) { add_text(s.substr(i)); break; }
        add_text(s.substr(i, lt - i));
        if (s.compare(lt, 4, "<!--") == 0) {
            auto e = s.find("-->", lt + 4);
            // keep the Markdown page-break marker and Word's own <!--[if ...]> is dropped
            std::string c = s.substr(lt + 4, e == std::string::npos ? std::string::npos : e - lt - 4);
            if (util::trim(c) == "pagebreak") {
                Node n;
                n.tag = "pagebreak";
                stack.back()->kids.push_back(n);
            }
            i = e == std::string::npos ? s.size() : e + 3;
            continue;
        }
        if (s.compare(lt, 2, "<!") == 0 || s.compare(lt, 2, "<?") == 0) {
            auto e = s.find('>', lt);
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        bool closing = lt + 1 < s.size() && s[lt + 1] == '/';
        std::size_t j = lt + (closing ? 2 : 1);
        std::string name;
        while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == ':' || s[j] == '-')) name += (char)std::tolower((unsigned char)s[j++]);
        if (name.empty()) { add_text("<"); i = lt + 1; continue; }
        // attributes
        std::map<std::string, std::string> attrs;
        bool self_close = false;
        while (j < s.size() && s[j] != '>') {
            if (std::isspace((unsigned char)s[j])) { j++; continue; }
            if (s[j] == '/') { self_close = true; j++; continue; }
            std::string an;
            while (j < s.size() && !std::isspace((unsigned char)s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/') an += (char)std::tolower((unsigned char)s[j++]);
            while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
            std::string av;
            if (j < s.size() && s[j] == '=') {
                j++;
                while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
                if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
                    char q = s[j++];
                    auto e = s.find(q, j);
                    av = s.substr(j, e == std::string::npos ? std::string::npos : e - j);
                    j = e == std::string::npos ? s.size() : e + 1;
                } else {
                    while (j < s.size() && !std::isspace((unsigned char)s[j]) && s[j] != '>') av += s[j++];
                }
            }
            if (!an.empty()) attrs[an] = decode_entities(av);
        }
        i = j < s.size() ? j + 1 : s.size();
        if (closing) {
            // pop to the matching open tag, if any
            for (int k = (int)stack.size() - 1; k > 0; k--) {
                if (stack[k]->tag == name) { stack.resize(k); break; }
            }
            continue;
        }
        if (name == "script" || name == "style" || name == "head" || name == "title" || name == "noscript" || name == "template") {
            auto e = util::lower(s).find("</" + name, i);
            i = e == std::string::npos ? s.size() : s.find('>', e) == std::string::npos ? s.size() : s.find('>', e) + 1;
            continue;
        }
        while (stack.size() > 1 && closes(stack.back()->tag, name)) stack.pop_back();
        Node n;
        n.tag = name;
        n.attrs = std::move(attrs);
        stack.back()->kids.push_back(std::move(n));
        Node* added = &stack.back()->kids.back();
        if (name == "pre" || name == "textarea") {
            // raw-ish content: keep whitespace, still parse tags inside (code spans)
        }
        if (!self_close && !void_tags().count(name)) stack.push_back(added);
    }
    return root;
}

// CSS declarations of a style attribute.
std::map<std::string, std::string> css(const std::string& style) {
    std::map<std::string, std::string> m;
    for (auto& decl : util::split(style, ';')) {
        auto c = decl.find(':');
        if (c == std::string::npos) continue;
        std::string k = util::lower(util::trim(decl.substr(0, c)));
        std::string v = util::trim(decl.substr(c + 1));
        if (auto imp = util::lower(v).find("!important"); imp != std::string::npos) v = util::trim(v.substr(0, imp));
        m[k] = v;
    }
    return m;
}

// CSS color -> RRGGBB, "" if unknown/transparent.
std::string css_color(std::string v) {
    v = util::lower(util::trim(v));
    if (v.empty() || v == "transparent" || v == "inherit" || v == "initial" || v == "currentcolor") return "";
    static const std::map<std::string, std::string> names = {
        {"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"}, {"green", "008000"}, {"blue", "0000FF"}, {"yellow", "FFFF00"},
        {"orange", "FFA500"}, {"purple", "800080"}, {"gray", "808080"}, {"grey", "808080"}, {"silver", "C0C0C0"}, {"maroon", "800000"},
        {"navy", "000080"}, {"teal", "008080"}, {"olive", "808000"}, {"lime", "00FF00"}, {"aqua", "00FFFF"}, {"cyan", "00FFFF"},
        {"fuchsia", "FF00FF"}, {"magenta", "FF00FF"}, {"pink", "FFC0CB"}, {"brown", "A52A2A"}, {"gold", "FFD700"},
        {"darkred", "8B0000"}, {"darkblue", "00008B"}, {"darkgreen", "006400"}, {"lightgray", "D3D3D3"}, {"lightgrey", "D3D3D3"},
        {"darkgray", "A9A9A9"}, {"darkgrey", "A9A9A9"}, {"lightblue", "ADD8E6"}, {"lightgreen", "90EE90"}, {"lightyellow", "FFFFE0"},
        {"crimson", "DC143C"}, {"indigo", "4B0082"}, {"violet", "EE82EE"}, {"coral", "FF7F50"}, {"tomato", "FF6347"},
        {"salmon", "FA8072"}, {"khaki", "F0E68C"}, {"beige", "F5F5DC"}, {"ivory", "FFFFF0"}, {"steelblue", "4682B4"},
        {"royalblue", "4169E1"}, {"slategray", "708090"}, {"whitesmoke", "F5F5F5"}, {"gainsboro", "DCDCDC"}, {"darkorange", "FF8C00"}};
    if (auto it = names.find(v); it != names.end()) return it->second;
    char buf[8];
    if (v[0] == '#') {
        std::string h = v.substr(1);
        if (h.size() == 3 || h.size() == 4) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
        if (h.size() == 8) h = h.substr(0, 6);
        if (h.size() != 6 || !std::all_of(h.begin(), h.end(), [](char c) { return std::isxdigit((unsigned char)c); })) return "";
        for (auto& c : h) c = (char)std::toupper((unsigned char)c);
        return h;
    }
    if (v.rfind("rgb", 0) == 0) {
        auto a = v.find('('), b = v.find(')');
        if (a == std::string::npos || b == std::string::npos) return "";
        std::string inner = v.substr(a + 1, b - a - 1);
        std::replace(inner.begin(), inner.end(), ',', ' ');
        std::replace(inner.begin(), inner.end(), '/', ' ');
        auto parts = util::split(inner, ' ');
        std::vector<double> n;
        for (auto& p : parts) {
            auto t = util::trim(p);
            if (t.empty()) continue;
            double x = util::to_double(t);
            if (t.back() == '%') x = x * 255 / 100;
            n.push_back(x);
        }
        if (n.size() < 3) return "";
        if (n.size() >= 4 && n[3] == 0) return "";  // fully transparent
        std::snprintf(buf, sizeof buf, "%02X%02X%02X", (int)std::clamp(std::lround(n[0]), 0L, 255L),
                      (int)std::clamp(std::lround(n[1]), 0L, 255L), (int)std::clamp(std::lround(n[2]), 0L, 255L));
        return buf;
    }
    return "";
}

// CSS length -> points; 0 if unknown. Relative sizes against 12pt.
double css_pt(std::string v, double base = 12) {
    v = util::lower(util::trim(v));
    static const std::map<std::string, double> kw = {{"xx-small", 7}, {"x-small", 7.5}, {"small", 10}, {"medium", 12},
                                                     {"large", 13.5}, {"x-large", 18}, {"xx-large", 24}, {"smaller", 10}, {"larger", 14}};
    if (auto it = kw.find(v); it != kw.end()) return it->second;
    double x = util::to_double(v, NAN);
    if (std::isnan(x)) return 0;
    if (v.find("px") != std::string::npos) return x * 0.75;
    if (v.find("pt") != std::string::npos) return x;
    if (v.find("rem") != std::string::npos || v.find("em") != std::string::npos) return x * base;
    if (v.find('%') != std::string::npos) return x * base / 100;
    if (v.find("in") != std::string::npos) return x * 72;
    if (v.find("cm") != std::string::npos) return x * 72 / 2.54;
    if (v.find("mm") != std::string::npos) return x * 72 / 25.4;
    return x * 0.75;  // unitless: pixels
}

class Converter {
public:
    std::vector<RichBlock> out;

    void block_children(const Node& n) {
        for (auto& k : n.kids) block(k);
        flush();
    }

private:
    std::vector<RichSpan> cur_;   // inline content of the paragraph being gathered
    Json cur_pfmt_ = Json::object();
    std::string cur_style_;
    int list_seq_ = 0;
    int quote_ = 0;

    void flush() {
        // drop whitespace-only paragraphs
        bool any = false;
        for (auto& s : cur_)
            if (!util::trim(s.text).empty() || s.fmt.contains("__br") || s.fmt.contains("__img")) any = true;
        if (any) {
            trim_spans(cur_);
            // a paragraph that was only line breaks
            bool text = false;
            for (auto& s : cur_) if (!util::trim(s.text).empty() || s.fmt.contains("__img")) text = true;
            if (!text) { cur_.clear(); cur_pfmt_ = Json::object(); cur_style_.clear(); return; }
            RichBlock b;
            b.kind = quote_ ? RichBlock::Quote : RichBlock::Para;
            b.spans = std::move(cur_);
            b.pfmt = cur_pfmt_;
            if (!quote_) b.style = cur_style_;
            out.push_back(std::move(b));
        }
        cur_.clear();
        cur_pfmt_ = Json::object();
        cur_style_.clear();
    }

    // Collapse HTML whitespace across spans; trim the ends of the paragraph.
    static void trim_spans(std::vector<RichSpan>& v) {
        bool prev_space = true;
        for (auto& s : v) {
            std::string o;
            for (char c : s.text) {
                if (c == '\n' && s.fmt.contains("__br")) { o += c; prev_space = true; continue; }
                bool sp = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
                if (sp) {
                    if (!prev_space) o += ' ';
                    prev_space = true;
                } else {
                    o += c;
                    prev_space = false;
                }
            }
            s.text = o;
            s.fmt.erase("__br");
        }
        while (!v.empty()) {
            if (v.back().fmt.contains("__img")) break;
            auto& t = v.back().text;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (t.empty() && v.back().link.empty()) v.pop_back();
            else break;
        }
        // drop leading spaces after line breaks and at the start
        bool at_start = true;
        for (auto& s : v) {
            if (at_start) {
                std::size_t k = 0;
                while (k < s.text.size() && s.text[k] == ' ') k++;
                s.text = s.text.substr(k);
            }
            if (!s.text.empty()) at_start = s.text.back() == '\n';
        }
    }

    static Json style_fmt(const std::map<std::string, std::string>& attrs, Json f) {
        auto it = attrs.find("style");
        if (it == attrs.end()) return f;
        auto c = css(it->second);
        if (c.count("color")) if (auto col = css_color(c["color"]); !col.empty()) f["color"] = col;
        for (auto key : {"background-color", "background"})
            if (c.count(key)) if (auto col = css_color(c[key]); !col.empty()) { f["shading"] = col; break; }
        if (c.count("font-weight")) {
            auto w = util::lower(c["font-weight"]);
            if (w == "bold" || w == "bolder" || util::to_int(w) >= 600) f["bold"] = true;
            else if (w == "normal" || w == "lighter" || (util::to_int(w) > 0 && util::to_int(w) < 600)) f.erase("bold");
        }
        if (c.count("font-style")) {
            auto s = util::lower(c["font-style"]);
            if (s == "italic" || s == "oblique") f["italic"] = true;
            else if (s == "normal") f.erase("italic");
        }
        for (auto key : {"text-decoration", "text-decoration-line"})
            if (c.count(key)) {
                auto s = util::lower(c[key]);
                if (s.find("underline") != std::string::npos) f["underline"] = true;
                if (s.find("line-through") != std::string::npos) f["strike"] = true;
            }
        if (c.count("font-size")) if (double pt = css_pt(c["font-size"]); pt >= 1 && pt <= 1638) f["size"] = std::round(pt * 2) / 2;
        if (c.count("font-family")) {
            auto fam = util::split(c["font-family"], ',');
            if (!fam.empty()) {
                std::string ff = util::trim(fam[0]);
                if (ff.size() >= 2 && (ff.front() == '"' || ff.front() == '\'')) ff = ff.substr(1, ff.size() - 2);
                static const std::map<std::string, std::string> generic = {{"monospace", "Consolas"}, {"serif", "Times New Roman"}, {"sans-serif", "Arial"}, {"system-ui", "Segoe UI"}, {"cursive", "Comic Sans MS"}};
                if (auto g = generic.find(util::lower(ff)); g != generic.end()) ff = g->second;
                if (!ff.empty() && ff.find_first_of("<>&") == std::string::npos) f["font"] = ff;
            }
        }
        if (c.count("vertical-align")) {
            auto s = util::lower(c["vertical-align"]);
            if (s == "super") f["superscript"] = true;
            else if (s == "sub") f["subscript"] = true;
        }
        if (c.count("text-transform") && util::lower(c["text-transform"]) == "uppercase") f["all_caps"] = true;
        if (c.count("font-variant") && util::lower(c["font-variant"]).find("small-caps") != std::string::npos) f["small_caps"] = true;
        return f;
    }

    static Json para_fmt(const std::map<std::string, std::string>& attrs) {
        Json p = Json::object();
        std::string align;
        if (auto a = attrs.find("align"); a != attrs.end()) align = util::lower(a->second);
        if (auto it = attrs.find("style"); it != attrs.end()) {
            auto c = css(it->second);
            if (c.count("text-align")) align = util::lower(c["text-align"]);
            if (c.count("margin-left")) if (double pt = css_pt(c["margin-left"]); pt > 0 && pt < 1000) p["left_indent"] = pt;
            if (c.count("padding-left") && !p.contains("left_indent")) if (double pt = css_pt(c["padding-left"]); pt > 0 && pt < 1000) p["left_indent"] = pt;
            if (c.count("text-indent")) if (double pt = css_pt(c["text-indent"]); std::fabs(pt) < 1000 && pt != 0) {
                if (pt > 0) p["first_line_indent"] = pt;
                else p["hanging_indent"] = -pt;
            }
            if (c.count("margin-top")) if (double pt = css_pt(c["margin-top"]); pt >= 0 && pt < 1000) p["space_before"] = pt;
            if (c.count("margin-bottom")) if (double pt = css_pt(c["margin-bottom"]); pt >= 0 && pt < 1000) p["space_after"] = pt;
            if (c.count("line-height")) {
                std::string lh = util::lower(c["line-height"]);
                double x = util::to_double(lh, NAN);
                if (!std::isnan(x) && lh.find_first_of("ptxem%") == std::string::npos && x > 0 && x < 10) p["line_spacing"] = x;
            }
            if (c.count("page-break-before") && util::lower(c["page-break-before"]) == "always") p["page_break_before"] = true;
            if (c.count("break-before") && util::lower(c["break-before"]) == "page") p["page_break_before"] = true;
        }
        if (align == "center") p["alignment"] = "center";
        else if (align == "right" || align == "end") p["alignment"] = "right";
        else if (align == "justify") p["alignment"] = "both";
        return p;
    }

    void inline_node(const Node& n, Json fmt, const std::string& link, std::vector<RichSpan>& into, bool pre) {
        if (n.tag.empty()) {
            std::string t = n.text;
            if (pre) {
                // keep newlines as line breaks
                std::string acc;
                for (char c : t) {
                    if (c == '\n') {
                        into.push_back({acc, fmt, link});
                        Json bf = fmt;
                        bf["__br"] = true;
                        into.push_back({"\n", bf, ""});
                        acc.clear();
                    } else acc += c == '\t' ? ' ' : c;
                }
                if (!acc.empty()) into.push_back({acc, fmt, link});
                return;
            }
            into.push_back({t, fmt, link});
            return;
        }
        const std::string& t = n.tag;
        if (t == "br") {
            Json bf = fmt;
            bf["__br"] = true;
            into.push_back({"\n", bf, ""});
            return;
        }
        if (t == "img") { inline_image(n, &into); return; }
        Json f = fmt;
        std::string l = link;
        if (t == "b" || t == "strong") f["bold"] = true;
        else if (t == "i" || t == "em" || t == "cite" || t == "dfn" || t == "var") f["italic"] = true;
        else if (t == "u" || t == "ins") f["underline"] = true;
        else if (t == "s" || t == "strike" || t == "del") f["strike"] = true;
        else if (t == "sup") f["superscript"] = true;
        else if (t == "sub") f["subscript"] = true;
        else if (t == "code" || t == "kbd" || t == "samp" || t == "tt") { f["font"] = "Consolas"; if (!pre) f["shading"] = "F2F2F2"; }
        else if (t == "mark") f["highlight"] = "yellow";
        else if (t == "small") f["size"] = 10;
        else if (t == "big") f["size"] = 14;
        else if (t == "font") {
            if (auto c = n.attrs.find("color"); c != n.attrs.end()) if (auto col = css_color(c->second); !col.empty()) f["color"] = col;
            if (auto c = n.attrs.find("face"); c != n.attrs.end()) f["font"] = util::trim(util::split(c->second, ',')[0]);
            if (auto c = n.attrs.find("size"); c != n.attrs.end()) {
                static const double sizes[] = {0, 7.5, 10, 12, 13.5, 18, 24, 36};
                int k = std::clamp(util::to_int(c->second), 1, 7);
                f["size"] = sizes[k];
            }
        } else if (t == "a") {
            auto h = n.attrs.find("href");
            if (h != n.attrs.end() && !h->second.empty() && h->second.rfind("javascript:", 0) != 0 && h->second[0] != '#') l = h->second;
        } else if (t == "q") {
            into.push_back({"\xE2\x80\x9C", f, l});
        }
        f = style_fmt(n.attrs, f);
        for (auto& k : n.kids) inline_node(k, f, l, into, pre || t == "pre");
        if (t == "q") into.push_back({"\xE2\x80\x9D", f, l});
    }

    void inline_image(const Node& n, std::vector<RichSpan>* into = nullptr) {
        auto src = n.attrs.find("src");
        if (src == n.attrs.end()) return;
        Json f = Json::object();
        f["__img"] = src->second;
        if (auto a = n.attrs.find("alt"); a != n.attrs.end()) f["__alt"] = a->second;
        double w = 0, h = 0;
        if (auto x = n.attrs.find("width"); x != n.attrs.end()) w = css_pt(x->second);
        if (auto x = n.attrs.find("height"); x != n.attrs.end()) h = css_pt(x->second);
        if (auto st = n.attrs.find("style"); st != n.attrs.end()) {
            auto c = css(st->second);
            if (c.count("width")) w = css_pt(c["width"]);
            if (c.count("height")) h = css_pt(c["height"]);
        }
        if (w > 0) f["__w"] = w;
        if (h > 0) f["__h"] = h;
        (into ? *into : cur_).push_back({"", f, ""});
    }

    static bool is_block(const std::string& t) {
        static const std::set<std::string> b = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "table", "pre", "blockquote",
                                                "hr", "section", "article", "header", "footer", "main", "nav", "aside", "figure", "figcaption",
                                                "dl", "dt", "dd", "address", "body", "html", "center", "details", "summary", "pagebreak", "form", "fieldset"};
        return b.count(t) > 0;
    }

    // Inline content of a block element straight into a span list.
    std::vector<RichSpan> spans_of(const Node& n, Json base = Json::object()) {
        std::vector<RichSpan> v;
        base = style_fmt(n.attrs, base);
        for (auto& k : n.kids) {
            if (!k.tag.empty() && is_block(k.tag) && k.tag != "pagebreak") {
                // nested block inside an inline context (div in li): a line break and its text
                if (!v.empty()) {
                    Json bf;
                    bf["__br"] = true;
                    v.push_back({"\n", bf, ""});
                }
                auto inner = spans_of(k, base);
                v.insert(v.end(), inner.begin(), inner.end());
                continue;
            }
            inline_node(k, base, "", v, n.tag == "pre");
        }
        return v;
    }

    void list(const Node& n, int level, bool numbered, int id) {
        for (auto& li : n.kids) {
            if (li.tag.empty()) continue;  // whitespace between items
            if (li.tag == "ul" || li.tag == "ol") { list(li, level + 1, li.tag == "ol", id); continue; }
            if (li.tag != "li") {
                // stray content in a list: treat as an item
            }
            // the item's own text, then nested lists
            std::vector<RichSpan> spans;
            std::vector<const Node*> nested;
            for (auto& k : li.kids) {
                if (k.tag == "ul" || k.tag == "ol") { nested.push_back(&k); continue; }
                if (!k.tag.empty() && (k.tag == "p" || k.tag == "div")) {
                    auto s = spans_of(k);
                    if (!spans.empty() && !s.empty()) { Json bf; bf["__br"] = true; spans.push_back({"\n", bf, ""}); }
                    spans.insert(spans.end(), s.begin(), s.end());
                    continue;
                }
                inline_node(k, style_fmt(li.attrs, Json::object()), "", spans, false);
            }
            trim_spans(spans);
            RichBlock b;
            b.kind = RichBlock::Item;
            b.level = std::min(level, 8);
            b.numbered = numbered;
            b.list = id;
            b.spans = spans;
            if (!spans.empty() || nested.empty()) out.push_back(b);
            for (auto* ns : nested) list(*ns, level + 1, ns->tag == "ol", id);
        }
    }

    void table(const Node& n) {
        RichBlock b;
        b.kind = RichBlock::Table;
        std::function<void(const Node&, bool)> rows = [&](const Node& x, bool head) {
            for (auto& k : x.kids) {
                if (k.tag == "thead") { rows(k, true); continue; }
                if (k.tag == "tbody" || k.tag == "tfoot") { rows(k, false); continue; }
                if (k.tag == "caption") continue;
                if (k.tag != "tr") continue;
                std::vector<std::vector<RichSpan>> cells;
                std::vector<Json> fmts;
                bool all_th = true, any = false;
                for (auto& c : k.kids) {
                    if (c.tag != "td" && c.tag != "th") continue;
                    any = true;
                    auto s = spans_of(c);
                    trim_spans(s);
                    cells.push_back(s);
                    Json cf = Json::object();
                    if (c.tag == "th") cf["header"] = true;
                    else all_th = false;
                    if (auto cs = c.attrs.find("colspan"); cs != c.attrs.end()) cf["colspan"] = std::clamp(util::to_int(cs->second, 1), 1, 63);
                    std::string bg;
                    if (auto a = c.attrs.find("bgcolor"); a != c.attrs.end()) bg = css_color(a->second);
                    if (auto st = c.attrs.find("style"); st != c.attrs.end()) {
                        auto cc = css(st->second);
                        for (auto key : {"background-color", "background"})
                            if (cc.count(key)) if (auto col = css_color(cc[key]); !col.empty()) bg = col;
                    }
                    if (!bg.empty()) cf["shading"] = bg;
                    auto pf = para_fmt(c.attrs);
                    if (pf.contains("alignment")) cf["alignment"] = pf["alignment"];
                    fmts.push_back(cf);
                }
                if (!any) continue;
                if (b.rows.empty() && (head || all_th)) b.header_row = true;
                b.rows.push_back(cells);
                b.cell_fmt.push_back(fmts);
            }
        };
        rows(n, false);
        // a caption becomes a paragraph above
        for (auto& k : n.kids)
            if (k.tag == "caption") {
                RichBlock cap;
                cap.kind = RichBlock::Para;
                cap.style = "Caption";
                cap.spans = spans_of(k);
                trim_spans(cap.spans);
                if (!cap.spans.empty()) out.push_back(cap);
            }
        if (!b.rows.empty()) out.push_back(b);
    }

    void block(const Node& n) {
        const std::string& t = n.tag;
        if (t.empty() || !is_block(t)) {
            if (t == "img") { inline_image(n); return; }
            if (t == "br") {
                Json bf;
                bf["__br"] = true;
                cur_.push_back({"\n", bf, ""});
                return;
            }
            inline_node(n, Json::object(), "", cur_, false);
            return;
        }
        flush();
        if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
            RichBlock b;
            b.kind = RichBlock::Heading;
            b.level = t[1] - '0';
            b.spans = spans_of(n);
            trim_spans(b.spans);
            b.pfmt = para_fmt(n.attrs);
            if (!b.spans.empty()) out.push_back(b);
            return;
        }
        if (t == "p" || t == "figcaption" || t == "dt" || t == "summary" || t == "address" || t == "center") {
            cur_pfmt_ = para_fmt(n.attrs);
            if (t == "center") cur_pfmt_["alignment"] = "center";
            if (t == "figcaption") cur_style_ = "Caption";
            // a <p> holding block children (bad HTML) still works: blocks flush around
            bool has_block = false;
            for (auto& k : n.kids) if (!k.tag.empty() && is_block(k.tag)) has_block = true;
            if (has_block) {
                for (auto& k : n.kids) block(k);
            } else {
                auto s = spans_of(n);
                cur_.insert(cur_.end(), s.begin(), s.end());
            }
            if (t == "dt") for (auto& s : cur_) s.fmt["bold"] = true;
            flush();
            return;
        }
        if (t == "dd") {
            cur_pfmt_ = para_fmt(n.attrs);
            cur_pfmt_["left_indent"] = 36;
            auto s = spans_of(n);
            cur_.insert(cur_.end(), s.begin(), s.end());
            flush();
            return;
        }
        if (t == "pre") {
            RichBlock b;
            b.kind = RichBlock::Code;
            std::vector<RichSpan> s;
            for (auto& k : n.kids) inline_node(k, Json::object(), "", s, true);
            std::string text;
            for (auto& x : s) text += x.text;
            // strip one leading and trailing newline
            if (!text.empty() && text.front() == '\n') text.erase(0, 1);
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            b.spans = {{text, Json::object(), ""}};
            out.push_back(b);
            return;
        }
        if (t == "blockquote") {
            quote_++;
            bool has_block = false;
            for (auto& k : n.kids) if (!k.tag.empty() && is_block(k.tag)) has_block = true;
            if (has_block) block_children(n);
            else {
                auto s = spans_of(n);
                cur_.insert(cur_.end(), s.begin(), s.end());
                flush();
            }
            quote_--;
            return;
        }
        if (t == "ul" || t == "ol") { list(n, 0, t == "ol", ++list_seq_); return; }
        if (t == "li") { list(Node{"ul", {}, "", {n}}, 0, false, ++list_seq_); return; }
        if (t == "table") { table(n); return; }
        if (t == "hr") { RichBlock b; b.kind = RichBlock::Rule; out.push_back(b); return; }
        if (t == "pagebreak") { RichBlock b; b.kind = RichBlock::PageBreak; out.push_back(b); return; }
        // containers: div, section, body, ... carry alignment / page breaks to their text
        Json pf = para_fmt(n.attrs);
        if (pf.value("page_break_before", false)) { RichBlock b; b.kind = RichBlock::PageBreak; out.push_back(b); pf.erase("page_break_before"); }
        bool has_block = false;
        for (auto& k : n.kids) if (!k.tag.empty() && is_block(k.tag)) has_block = true;
        if (!has_block) {
            cur_pfmt_ = pf;
            auto s = spans_of(n);
            cur_.insert(cur_.end(), s.begin(), s.end());
            flush();
            return;
        }
        for (auto& k : n.kids) {
            if (!k.tag.empty() && is_block(k.tag)) { flush(); block(k); }
            else {
                if (cur_.empty()) cur_pfmt_ = pf;
                inline_node(k, style_fmt(n.attrs, Json::object()), "", cur_, false);
            }
        }
        flush();
    }
};

}  // namespace

std::vector<RichBlock> parse_html(const std::string& html) {
    // CF_HTML clipboard: keep only the fragment
    std::string s = html;
    auto a = s.find("<!--StartFragment-->"), b = s.find("<!--EndFragment-->");
    if (a != std::string::npos && b != std::string::npos && b > a) s = s.substr(a + 20, b - a - 20);
    Node root = parse_tree(s);
    Converter c;
    c.block_children(root);
    return c.out;
}

}  // namespace docx::wml
