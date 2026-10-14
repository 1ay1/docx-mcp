// Markdown to Word: headings, paragraphs with **bold** *italic* `code`
// ~~strike~~ [links](url), bullet and numbered lists (nested), block quotes,
// fenced code, tables, horizontal rules and page breaks. Built from the same
// primitives as the other tools, so tracking and validity come with it.
#include "wml/markdown.hpp"

#include <stdexcept>

#include "util/util.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "schema/schema.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

struct Span {
    std::string text;
    bool bold = false, italic = false, code = false, strike = false;
    std::string link;
};

// Inline markdown to spans. Unclosed markers stay literal.
std::vector<Span> inline_spans(const std::string& s) {
    std::vector<Span> out;
    Span cur;
    bool bold = false, italic = false, strike = false;
    auto flush = [&] {
        if (!cur.text.empty()) out.push_back(cur);
        cur = Span{};
        cur.bold = bold;
        cur.italic = italic;
        cur.strike = strike;
    };
    auto closes = [&](std::size_t from, const std::string& m) { return s.find(m, from) != std::string::npos; };
    std::size_t i = 0;
    flush();
    while (i < s.size()) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::string("\\`*_~[]()#+-.!|>").find(s[i + 1]) != std::string::npos) {
            cur.text += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '`') {
            auto e = s.find('`', i + 1);
            if (e != std::string::npos) {
                flush();
                Span sp = cur;
                sp.text = s.substr(i + 1, e - i - 1);
                sp.code = true;
                out.push_back(sp);
                i = e + 1;
                continue;
            }
        }
        if (c == '[') {
            auto rb = s.find("](", i + 1);
            auto rp = rb == std::string::npos ? rb : s.find(')', rb + 2);
            if (rb != std::string::npos && rp != std::string::npos && s.find('\n', i) > rp) {
                flush();
                for (auto& inner : inline_spans(s.substr(i + 1, rb - i - 1))) {
                    inner.bold |= bold;
                    inner.italic |= italic;
                    inner.strike |= strike;
                    inner.link = s.substr(rb + 2, rp - rb - 2);
                    out.push_back(inner);
                }
                i = rp + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            std::string m(2, c);
            if (bold || closes(i + 2, m)) {
                flush();
                bold = !bold;
                cur.bold = bold;
                i += 2;
                continue;
            }
        }
        if (c == '~' && i + 1 < s.size() && s[i + 1] == '~') {
            if (strike || closes(i + 2, "~~")) {
                flush();
                strike = !strike;
                cur.strike = strike;
                i += 2;
                continue;
            }
        }
        if (c == '*' || (c == '_' && (i == 0 || !std::isalnum((unsigned char)s[i - 1])))) {
            if (italic || closes(i + 1, std::string(1, c))) {
                flush();
                italic = !italic;
                cur.italic = italic;
                i += 1;
                continue;
            }
        }
        cur.text += c;
        i++;
    }
    flush();
    return out;
}

std::string strip(const std::string& s) { return std::string(util::trim(s)); }

int indent_of(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') n++;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// "- x", "* x", "+ x" -> bullet; "1. x", "1) x" -> number. Returns marker length.
std::size_t list_marker(const std::string& t, bool& numbered) {
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        numbered = false;
        return 2;
    }
    std::size_t i = 0;
    while (i < t.size() && std::isdigit((unsigned char)t[i])) i++;
    if (i > 0 && i < 10 && i + 1 < t.size() && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ') {
        numbered = true;
        return i + 2;
    }
    return 0;
}

bool is_rule(const std::string& t) {
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char x : t) if (x != c && x != ' ') return false;
    return true;
}

bool is_table_sep(const std::string& t) {
    if (t.find('-') == std::string::npos || t.find('|') == std::string::npos) return false;
    for (char c : t) if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return true;
}

std::vector<std::string> table_cells(std::string t) {
    t = strip(t);
    if (!t.empty() && t.front() == '|') t.erase(0, 1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    for (std::size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += '|'; i++; continue; }
        if (t[i] == '|') { cells.push_back(strip(cur)); cur.clear(); continue; }
        cur += t[i];
    }
    cells.push_back(strip(cur));
    return cells;
}

class Writer {
public:
    // Builds untracked; insert_markdown marks the result inserted if tracking.
    Writer(Document& d, pugi::xml_node ref, bool after) : d_(d), ref_(ref), after_(after), o_{false} {}

    // A new paragraph after the previous one, filled with inline spans.
    pugi::xml_node para(const std::string& style, const std::string& md, const Json& pfmt = nullptr) {
        auto p = fresh(style.empty() ? "Normal" : style, U"");
        fill(p, md);
        if (pfmt.is_object() && !pfmt.empty()) format_paragraph(d_, p, pfmt, o_);
        made_.push_back(p);
        return p;
    }

    pugi::xml_node code_para(const std::string& text) {
        auto p = fresh(code_style(), util::to_u32(text));
        made_.push_back(p);
        return p;
    }

    void table(const std::vector<std::vector<std::string>>& rows, const std::vector<std::string>& align) {
        std::vector<std::vector<std::string>> blank(rows.size(), std::vector<std::string>(rows.empty() ? 0 : rows[0].size()));
        pugi::xml_node next_before = after_ ? ref_.next_sibling() : ref_;
        auto tbl = insert_table(d_, ref_, after_, blank, Json{{"header_row", true}}, o_);
        // insert_table adds an empty paragraph after a final table; it's ours too
        if (auto np = tbl.next_sibling(); np && np != next_before && std::string_view(np.name()) == "w:p")
            made_.push_back(np);
        // cells get the same inline formatting as paragraphs
        int ri = 0;
        for (auto tr : xml::children(tbl, "w:tr")) {
            int ci = 0;
            for (auto tc : xml::children(tr, "w:tc")) {
                auto p = tc.child("w:p");
                if (p && ri < (int)rows.size() && ci < (int)rows[ri].size()) {
                    fill(p, rows[ri][ci], ri == 0);
                    if (ci < (int)align.size() && !align[ci].empty())
                        apply_ppr_json(d_, schema::ensure_child(p, "w:pPr"), Json{{"alignment", align[ci]}});
                }
                ci++;
            }
            ri++;
        }
        ref_ = tbl;
        after_ = true;
        tables_++;
        table_nodes_.push_back(tbl);
    }

    void rule() {
        auto p = para("", "");
        format_paragraph(d_, p, Json{{"borders", {{"bottom", {{"style", "single"}, {"size", 0.75}, {"color", "auto"}}}}}}, o_);
    }

    void page_break() {
        auto p = para("", "");
        insert_break(d_, p, 0, "page", o_);
    }

    std::vector<pugi::xml_node>& made() { return made_; }
    std::vector<pugi::xml_node>& table_nodes() { return table_nodes_; }
    int tables() const { return tables_; }
    pugi::xml_node last() const { return ref_; }

private:
    // A paragraph with only the given style: none of the neighbour's
    // borders, numbering or run formatting.
    pugi::xml_node fresh(const std::string& style, const std::u32string& text) {
        auto p = insert_paragraph(d_, ref_, after_, text, style, nullptr, o_);
        if (auto ppr = p.child("w:pPr")) {
            std::vector<pugi::xml_node> drop;
            for (auto c : ppr.children()) {
                std::string n = c.name();
                if (n != "w:pStyle" && n != "w:rPr" && n != "w:sectPr") drop.push_back(c);
            }
            for (auto c : drop) ppr.remove_child(c);
            if (auto mark = ppr.child("w:rPr")) {
                std::vector<pugi::xml_node> dropr;
                for (auto c : mark.children()) {
                    std::string n = c.name();
                    if (n != "w:ins" && n != "w:del") dropr.push_back(c);
                }
                for (auto c : dropr) mark.remove_child(c);
                if (!mark.first_child()) ppr.remove_child(mark);
            }
            if (!ppr.first_child()) p.remove_child(ppr);
        }
        // runs carry no formatting of their own
        for (auto r : p.select_nodes(".//w:r")) r.node().remove_child("w:rPr");
        ref_ = p;
        after_ = true;
        return p;
    }

    std::string code_style() {
        if (code_style_.empty()) code_style_ = ensure_style(d_, "HTMLPreformatted");
        return code_style_;
    }

    static Json span_fmt(const Span& sp) {
        Json f = Json::object();
        if (sp.bold) f["bold"] = true;
        if (sp.italic) f["italic"] = true;
        if (sp.strike) f["strike"] = true;
        if (sp.code) {
            f["font"] = "Consolas";
            f["shading"] = "F2F2F2";
        }
        return f;
    }

    // Append md's spans to an (empty) paragraph.
    void fill(pugi::xml_node p, const std::string& md, bool bold_all = false) {
        int pos = (int)analyze(p).vis.size();
        for (auto sp : inline_spans(md)) {
            if (sp.text.empty()) continue;
            sp.bold |= bold_all;
            auto u = util::to_u32(sp.text);
            Json f = span_fmt(sp);
            if (!sp.link.empty()) {
                add_hyperlink(d_, p, pos, pos, sp.link, sp.text, "", o_);
                int end = pos + (int)u.size();
                Json lf = f;
                if (lf.contains("font")) lf.erase("shading");
                if (!lf.empty()) format_runs(d_, p, pos, end, lf, o_);
                pos = end;
                continue;
            }
            // A paragraph being built is only ever appended to, so each span
            // is a new run at the end, outside any link before it.
            auto r = p.append_child("w:r");
            append_text(r, u);
            if (!f.empty()) apply_rpr_json(d_, schema::ensure_child(r, "w:rPr"), f);
            pos += (int)u.size();
        }
    }


    Document& d_;
    pugi::xml_node ref_;
    bool after_;
    EditOpts o_;
    std::vector<pugi::xml_node> made_;
    std::vector<pugi::xml_node> table_nodes_;
    std::string code_style_;
    int tables_ = 0;
};

}  // namespace

MarkdownResult insert_markdown(Document& d, pugi::xml_node ref, bool after, const std::string& md, const EditOpts& o) {
    Writer w(d, ref, after);
    auto lines = util::split(md, '\n');
    for (auto& l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    MarkdownResult res;

    // list state: one numId per run of list items, per kind
    struct ListRun { std::vector<pugi::xml_node> paras; std::vector<int> levels; bool numbered; };
    std::vector<ListRun> runs;
    bool in_list = false;
    auto end_list = [&] { in_list = false; };

    std::string para_buf;
    std::string quote_buf;
    auto flush_para = [&] {
        if (!util::trim(para_buf).empty()) w.para("", strip(para_buf));
        para_buf.clear();
    };
    auto flush_quote = [&] {
        if (!util::trim(quote_buf).empty()) w.para("Quote", strip(quote_buf));
        quote_buf.clear();
    };

    for (std::size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];
        std::string t = strip(line);

        // fenced code
        if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) {
            flush_para(); flush_quote(); end_list();
            std::string fence = t.substr(0, 3);
            for (i++; i < lines.size() && strip(lines[i]).rfind(fence, 0) != 0; i++) w.code_para(lines[i]);
            continue;
        }
        if (t.empty()) {
            flush_para(); flush_quote(); end_list();
            continue;
        }
        // page break marker
        if (t == "\\pagebreak" || t == "<!-- pagebreak -->" || t == "\\newpage") {
            flush_para(); flush_quote(); end_list();
            w.page_break();
            continue;
        }
        // headings: # .. ######
        if (t[0] == '#') {
            std::size_t h = 0;
            while (h < t.size() && t[h] == '#') h++;
            if (h <= 6 && h < t.size() && t[h] == ' ') {
                flush_para(); flush_quote(); end_list();
                std::string text = strip(t.substr(h));
                while (!text.empty() && text.back() == '#') text.pop_back();
                w.para(ensure_style(d, "Heading" + std::to_string(h)), strip(text));
                continue;
            }
        }
        // setext headings
        if (!para_buf.empty() && para_buf.find('\n') == std::string::npos && (t.find_first_not_of('=') == std::string::npos || (t.find_first_not_of('-') == std::string::npos && t.size() >= 2))) {
            std::string text = strip(para_buf);
            para_buf.clear();
            w.para(ensure_style(d, t[0] == '=' ? "Heading1" : "Heading2"), text);
            continue;
        }
        if (is_rule(t)) {
            flush_para(); flush_quote(); end_list();
            w.rule();
            continue;
        }
        // tables: header row, separator, rows
        if (t.find('|') != std::string::npos && i + 1 < lines.size() && is_table_sep(strip(lines[i + 1]))) {
            flush_para(); flush_quote(); end_list();
            std::vector<std::vector<std::string>> rows{table_cells(t)};
            std::size_t ncol = rows[0].size();
            std::vector<std::string> align;
            for (auto& c : table_cells(lines[i + 1])) {
                bool l = !c.empty() && c.front() == ':', r = !c.empty() && c.back() == ':';
                align.push_back(l && r ? "center" : r ? "right" : "");
            }
            for (i += 2; i < lines.size() && strip(lines[i]).find('|') != std::string::npos; i++) {
                auto r = table_cells(lines[i]);
                r.resize(ncol);
                rows.push_back(r);
            }
            i--;
            w.table(rows, align);
            continue;
        }
        // block quote
        if (t[0] == '>') {
            flush_para(); end_list();
            std::string q = strip(t.substr(1));
            quote_buf += (quote_buf.empty() ? "" : " ") + q;
            continue;
        }
        flush_quote();
        // list items
        bool numbered = false;
        if (auto ml = list_marker(t, numbered)) {
            flush_para();
            int level = std::min(8, indent_of(line) / 2);
            std::string item = t.substr(ml);
            // task list
            if (item.rfind("[ ] ", 0) == 0) item = "\xE2\x98\x90 " + item.substr(4);
            else if (item.rfind("[x] ", 0) == 0 || item.rfind("[X] ", 0) == 0) item = "\xE2\x98\x92 " + item.substr(4);
            if (!in_list || runs.back().numbered != numbered && level == 0) {
                runs.push_back({{}, {}, numbered});
                in_list = true;
            }
            auto p = w.para("", item);
            runs.back().paras.push_back(p);
            runs.back().levels.push_back(level);
            continue;
        }
        // continuation of a list item (indented text)
        if (in_list && indent_of(line) >= 2 && !runs.back().paras.empty()) {
            auto p = runs.back().paras.back();
            int pos = (int)analyze(p).vis.size();
            insert_text(d, p, pos, util::to_u32(" " + t), Json::object(), {false});
            continue;
        }
        end_list();
        para_buf += (para_buf.empty() ? "" : "\n") + t;
    }
    flush_para();
    flush_quote();

    // lists last, so each run gets its own numbering that restarts at 1
    for (auto& r : runs) {
        int num_id = 0;
        for (std::size_t k = 0; k < r.paras.size(); k++) {
            Json opts = {{"continue_previous", false}};
            if (num_id) opts["num_id"] = num_id;
            num_id = apply_list(d, {r.paras[k]}, r.numbered ? "number" : "bullet", r.levels[k], opts, {false});
        }
        res.lists++;
    }
    bool tracked = o.track ? *o.track : d.track_changes();
    if (tracked) {
        for (auto p : w.made()) mark_paragraph_inserted(d, p);
        for (auto tbl : w.table_nodes()) {
            for (auto tr : xml::children(tbl, "w:tr")) {
                stamp(d, schema::ensure_child(schema::ensure_child(tr, "w:trPr"), "w:ins"));
                for (auto cp : tr.select_nodes(".//w:p")) mark_paragraph_inserted(d, cp.node());
            }
        }
    }
    for (auto p : w.made()) res.paragraphs.push_back(p);
    res.tables = w.tables();
    res.last = w.last();
    return res;
}

}  // namespace docx::wml
