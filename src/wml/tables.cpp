// Tables (ECMA 17.4) with Word's insert/delete/merge semantics.
#include <map>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool tracking(Document& d, const EditOpts& o) { return o.track ? *o.track : d.track_changes(); }

long twips_of(const Json& v) { return util::length_twips(v); }

// Text width of the section the node is in (page width - margins), twips.
long text_width(Document& d, pugi::xml_node near) {
    pugi::xml_node sect;
    for (auto n = near; n; n = n.next_sibling()) {
        if (auto s = n.child("w:pPr").child("w:sectPr")) { sect = s; break; }
    }
    if (!sect) sect = d.body().child("w:sectPr");
    long w = util::to_long(xml::attr(sect.child("w:pgSz"), "w:w", "12240"));
    long l = util::to_long(xml::attr(sect.child("w:pgMar"), "w:left", "1440"));
    long r = util::to_long(xml::attr(sect.child("w:pgMar"), "w:right", "1440"));
    return std::max(720L, w - l - r);
}

int span_of(pugi::xml_node tc) { return std::max(1, util::to_int(xml::attr(tc.child("w:tcPr").child("w:gridSpan"), "w:val", "1"))); }

pugi::xml_node new_cell(Document& d, pugi::xml_node tr, pugi::xml_node before, long width, const std::string& text) {
    auto tc = before ? tr.insert_child_before("w:tc", before) : tr.append_child("w:tc");
    auto tcpr = tc.append_child("w:tcPr");
    auto w = tcpr.append_child("w:tcW");
    xml::set_attr(w, "w:w", std::to_string(width));
    xml::set_attr(w, "w:type", "dxa");
    auto p = tc.append_child("w:p");
    p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
    p.append_attribute("w14:textId") = "77777777";
    if (!text.empty()) append_text(p.append_child("w:r"), util::to_u32(text));
    return tc;
}

void mark_row(Document& d, pugi::xml_node tr, const char* q) {
    auto trpr = schema::ensure_child(tr, "w:trPr");
    stamp(d, schema::ensure_child(trpr, q));
}

// Track every run and paragraph mark inside a node as inserted.
void track_inserted_content(Document& d, pugi::xml_node n) {
    for (auto p : xml::descendants(n, "w:p")) {
        stamp(d, schema::ensure_child(schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:rPr"), "w:ins"));
        std::vector<pugi::xml_node> runs;
        for (auto r : p.children("w:r")) runs.push_back(r);
        for (auto r : runs) {
            auto ins = p.insert_child_before("w:ins", r);
            stamp(d, ins);
            ins.append_move(r);
        }
    }
}

void track_deleted_content(Document& d, pugi::xml_node n) {
    for (auto p : xml::descendants(n, "w:p")) {
        int len = (int)analyze(p).vis.size();
        if (len) delete_text(d, p, 0, len, {true});
        auto rpr = schema::ensure_child(schema::ensure_child(p, "w:pPr"), "w:rPr");
        if (!rpr.child("w:del")) stamp(d, schema::ensure_child(rpr, "w:del"));
    }
}

}  // namespace

std::vector<pugi::xml_node> tables(Document& d) {
    std::vector<pugi::xml_node> v;
    for (auto b : blocks(d.body())) if (std::string_view(b.name()) == "w:tbl") v.push_back(b);
    return v;
}

pugi::xml_node table_at(Document& d, int index) {
    auto ts = tables(d);
    if (index < 1 || index > (int)ts.size())
        throw std::runtime_error("table " + std::to_string(index) + " not found (document has " + std::to_string(ts.size()) + ")");
    return ts[index - 1];
}

pugi::xml_node cell(pugi::xml_node tbl, int row, int col) {
    int r = 0;
    for (auto tr : tbl.children("w:tr")) {
        if (r++ != row) continue;
        int c = 0;
        for (auto tc : tr.children("w:tc")) {
            int span = span_of(tc);
            if (col >= c && col < c + span) return tc;
            c += span;
        }
        throw std::runtime_error("column " + std::to_string(col + 1) + " out of range");
    }
    throw std::runtime_error("row " + std::to_string(row + 1) + " out of range");
}

pugi::xml_node insert_table(Document& d, pugi::xml_node ref, bool after, const std::vector<std::vector<std::string>>& rows,
                            const Json& opts_in, const EditOpts& o) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    if (rows.empty()) throw std::runtime_error("table needs at least one row");
    std::size_t ncols = 0;
    for (auto& r : rows) ncols = std::max(ncols, r.size());
    if (!ncols) ncols = 1;
    auto parent = ref.parent();
    auto tbl = after ? parent.insert_child_after("w:tbl", ref) : parent.insert_child_before("w:tbl", ref);
    auto tblpr = tbl.append_child("w:tblPr");
    std::string style = opts.value("style", std::string("TableGrid"));
    if (!style.empty() && style != "none") xml::set_attr(tblpr.append_child("w:tblStyle"), "w:val", ensure_style(d, style));
    auto tw = tblpr.append_child("w:tblW");
    xml::set_attr(tw, "w:w", "0");
    xml::set_attr(tw, "w:type", "auto");
    if (opts.contains("alignment")) enum_attr(schema::ensure_child(tblpr, "w:jc"), "w:val", opts["alignment"].get<std::string>());
    auto look = schema::ensure_child(tblpr, "w:tblLook");
    bool hdr = opts.value("header_row", true);
    // Word's default look: first row + first column + banded rows
    xml::set_attr(look, "w:val", "04A0");
    xml::set_attr(look, "w:firstRow", hdr ? "1" : "0");
    xml::set_attr(look, "w:lastRow", "0");
    xml::set_attr(look, "w:firstColumn", opts.value("first_column", true) ? "1" : "0");
    xml::set_attr(look, "w:lastColumn", "0");
    xml::set_attr(look, "w:noHBand", opts.value("banded_rows", true) ? "0" : "1");
    xml::set_attr(look, "w:noVBand", "1");
    // widths
    std::vector<long> widths(ncols);
    if (opts.contains("widths")) {
        for (std::size_t i = 0; i < ncols; i++) widths[i] = i < opts["widths"].size() ? twips_of(opts["widths"][i]) : 1440;
    } else {
        long total = text_width(d, ref);
        for (auto& w : widths) w = total / (long)ncols;
    }
    auto grid = tbl.append_child("w:tblGrid");
    for (auto w : widths) xml::set_attr(grid.append_child("w:gridCol"), "w:w", std::to_string(w));
    for (std::size_t r = 0; r < rows.size(); r++) {
        auto tr = tbl.append_child("w:tr");
        tr.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        tr.append_attribute("w14:textId") = "77777777";
        if (r == 0 && hdr && opts.value("repeat_header", false)) schema::ensure_child(schema::ensure_child(tr, "w:trPr"), "w:tblHeader");
        for (std::size_t c = 0; c < ncols; c++) new_cell(d, tr, {}, widths[c], c < rows[r].size() ? rows[r][c] : "");
    }
    // A table can't be the last block of a body or cell: Word keeps a paragraph after it.
    if (!tbl.next_sibling("w:p") && !tbl.next_sibling("w:tbl") && !tbl.next_sibling("w:sdt")) {
        auto p = parent.insert_child_after("w:p", tbl);
        p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.append_attribute("w14:textId") = "77777777";
    }
    if (tracking(d, o)) {
        for (auto tr : tbl.children("w:tr")) mark_row(d, tr, "w:ins");
        track_inserted_content(d, tbl);
    }
    xml::declare(xml::root_of(tbl), "w14", true);
    if (opts.contains("format")) format_table(d, tbl, opts["format"], {false});
    return tbl;
}

void set_cell_text(Document& d, pugi::xml_node tc, const std::string& text, const EditOpts& o) {
    auto ps = xml::children(tc, "w:p");
    if (ps.empty()) throw std::runtime_error("cell has no paragraph");
    // delete content of all paragraphs, keep the first
    for (std::size_t i = 0; i < ps.size(); i++) {
        int n = (int)analyze(ps[i]).vis.size();
        if (n) delete_text(d, ps[i], 0, n, o);
        if (i > 0) delete_paragraph(d, ps[i], o);
    }
    auto lines = util::split(text, '\n');
    if (!lines.empty() && !lines[0].empty()) insert_text(d, ps[0], (int)analyze(ps[0]).vis.size(), util::to_u32(lines[0]), nullptr, o);
    pugi::xml_node cur = ps[0];
    for (std::size_t i = 1; i < lines.size(); i++)
        cur = insert_paragraph(d, cur, true, util::to_u32(lines[i]), "", nullptr, o);
}

pugi::xml_node insert_row(Document& d, pugi::xml_node tbl, int at, bool after, const std::vector<std::string>& cells, const EditOpts& o) {
    auto rows = xml::children(tbl, "w:tr");
    if (rows.empty()) throw std::runtime_error("table has no rows");
    if (at < 0 || at >= (int)rows.size()) throw std::runtime_error("row out of range");
    auto ref = rows[at];
    // Word copies the reference row's structure (cell widths, spans, props), empty
    auto nr = after ? tbl.insert_copy_after(ref, ref) : tbl.insert_copy_before(ref, ref);
    xml::set_attr(nr, "w14:paraId", d.new_hex_id());
    if (auto trpr = nr.child("w:trPr")) {
        for (auto q : {"w:ins", "w:del", "w:trPrChange", "w:tblHeader"}) trpr.remove_child(q);
        if (!trpr.first_child()) nr.remove_child(trpr);
    }
    int ci = 0;
    for (auto tc : xml::children(nr, "w:tc")) {
        if (auto tcpr = tc.child("w:tcPr")) {
            for (auto q : {"w:cellIns", "w:cellDel", "w:cellMerge", "w:tcPrChange"}) tcpr.remove_child(q);
            // vertical merges continue rather than restart in the new row
            if (auto vm = tcpr.child("w:vMerge"); vm && xml::attr(vm, "w:val") == "restart") xml::remove_attr(vm, "w:val");
        }
        std::vector<pugi::xml_node> drop;
        for (auto k : tc.children()) if (std::string_view(k.name()) != "w:tcPr") drop.push_back(k);
        pugi::xml_node keep_ppr;
        if (!drop.empty() && std::string_view(drop[0].name()) == "w:p") keep_ppr = drop[0].child("w:pPr");
        auto p = tc.append_child("w:p");
        if (keep_ppr) {
            auto c = p.append_copy(keep_ppr);
            c.remove_child("w:pPrChange");
            if (auto r = c.child("w:rPr")) { r.remove_child("w:ins"); r.remove_child("w:del"); }
        }
        for (auto k : drop) tc.remove_child(k);
        p.prepend_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.insert_attribute_after("w14:textId", p.attribute("w14:paraId")) = "77777777";
        if (ci < (int)cells.size() && !cells[ci].empty()) {
            auto r = p.append_child("w:r");
            copy_mark_rpr(r, p);
            append_text(r, util::to_u32(cells[ci]));
        }
        ci++;
    }
    if (tracking(d, o)) {
        mark_row(d, nr, "w:ins");
        track_inserted_content(d, nr);
    }
    return nr;
}

void delete_row(Document& d, pugi::xml_node tbl, int row, const EditOpts& o) {
    auto rows = xml::children(tbl, "w:tr");
    if (row < 0 || row >= (int)rows.size()) throw std::runtime_error("row out of range");
    auto tr = rows[row];
    if (!tracking(d, o)) {
        tbl.remove_child(tr);
        if (!tbl.child("w:tr")) delete_table(d, tbl, o);
        return;
    }
    if (auto ins = tr.child("w:trPr").child("w:ins"); ins && xml::attr(ins, "w:author") == d.author.name) {
        tbl.remove_child(tr);
        return;
    }
    mark_row(d, tr, "w:del");
    track_deleted_content(d, tr);
}

void insert_column(Document& d, pugi::xml_node tbl, int at, bool after, const EditOpts& o) {
    auto grid = tbl.child("w:tblGrid");
    auto cols = xml::children(grid, "w:gridCol");
    if (at < 0 || at >= (int)cols.size()) throw std::runtime_error("column out of range");
    long w = util::to_long(xml::attr(cols[at], "w:w", "1440"));
    // Word splits the reference column's width
    long half = std::max(1L, w / 2);
    xml::set_attr(cols[at], "w:w", std::to_string(w - half));
    auto nc = after ? grid.insert_copy_after(cols[at], cols[at]) : grid.insert_copy_before(cols[at], cols[at]);
    xml::set_attr(nc, "w:w", std::to_string(half));
    for (auto tr : tbl.children("w:tr")) {
        int c = 0;
        for (auto tc : xml::children(tr, "w:tc")) {
            int span = span_of(tc);
            if (at >= c && at < c + span) {
                auto tcw = tc.child("w:tcPr").child("w:tcW");
                if (tcw && span == 1) xml::set_attr(tcw, "w:w", std::to_string(w - half));
                if (span > 1) {
                    // widen the spanning cell instead of inserting
                    xml::set_attr(schema::ensure_child(schema::ensure_child(tc, "w:tcPr"), "w:gridSpan"), "w:val", std::to_string(span + 1));
                } else {
                    auto ncell = new_cell(d, tr, after ? tc.next_sibling("w:tc") : tc, half, "");
                    if (tracking(d, o)) {
                        stamp(d, schema::ensure_child(ncell.child("w:tcPr"), "w:cellIns"));
                        track_inserted_content(d, ncell);
                    }
                }
                break;
            }
            c += span;
        }
    }
}

void delete_column(Document& d, pugi::xml_node tbl, int col, const EditOpts& o) {
    auto grid = tbl.child("w:tblGrid");
    auto cols = xml::children(grid, "w:gridCol");
    if (col < 0 || col >= (int)cols.size()) throw std::runtime_error("column out of range");
    bool track = tracking(d, o);
    int removed = 0, rows = 0;
    for (auto tr : tbl.children("w:tr")) {
        rows++;
        int c = 0;
        for (auto tc : xml::children(tr, "w:tc")) {
            int span = span_of(tc);
            if (col >= c && col < c + span) {
                auto ci = tc.child("w:tcPr").child("w:cellIns");
                if (span > 1) {
                    xml::set_attr(tc.child("w:tcPr").child("w:gridSpan"), "w:val", std::to_string(span - 1));
                } else if (!track || (ci && xml::attr(ci, "w:author") == d.author.name)) {
                    // untracked, or deleting a cell this author inserted: it just goes
                    tr.remove_child(tc);
                    removed++;
                } else {
                    // cellIns/cellDel/cellMerge are a schema choice: a deletion replaces the insertion mark
                    auto tcpr = schema::ensure_child(tc, "w:tcPr");
                    tcpr.remove_child("w:cellIns");
                    tcpr.remove_child("w:cellMerge");
                    stamp(d, schema::ensure_child(tcpr, "w:cellDel"));
                    track_deleted_content(d, tc);
                }
                break;
            }
            c += span;
        }
    }
    if (!track || removed == rows) grid.remove_child(cols[col]);
    if (!xml::first_descendant(tbl, "w:tc")) {
        EditOpts untracked;
        untracked.track = false;
        delete_table(d, tbl, untracked);
    }
}

void merge_cells(Document& d, pugi::xml_node tbl, int r1, int c1, int r2, int c2) {
    if (r2 < r1) std::swap(r1, r2);
    if (c2 < c1) std::swap(c1, c2);
    auto rows = xml::children(tbl, "w:tr");
    if (r2 >= (int)rows.size()) throw std::runtime_error("row out of range");
    for (int r = r1; r <= r2; r++) {
        auto tr = rows[r];
        // find cells covering [c1, c2]
        std::vector<pugi::xml_node> covered;
        int c = 0, first_start = -1, last_end = -1;
        for (auto tc : xml::children(tr, "w:tc")) {
            int span = span_of(tc);
            if (c + span - 1 >= c1 && c <= c2) {
                covered.push_back(tc);
                if (first_start < 0) first_start = c;
                last_end = c + span - 1;
            }
            c += span;
        }
        if (covered.empty()) throw std::runtime_error("column out of range");
        if (first_start != c1 || last_end != c2) throw std::runtime_error("merge range cuts through a merged cell");
        auto keep = covered[0];
        long width = 0;
        for (auto tc : covered) width += util::to_long(xml::attr(tc.child("w:tcPr").child("w:tcW"), "w:w", "0"));
        // horizontal: Word appends the other cells' paragraphs to the first (non-empty ones)
        for (std::size_t i = 1; i < covered.size(); i++) {
            for (auto p : xml::children(covered[i], "w:p")) {
                if (analyze(p).vis.empty()) continue;
                keep.append_move(p);
            }
            tr.remove_child(covered[i]);
        }
        auto tcpr = schema::ensure_child(keep, "w:tcPr");
        if (c2 > c1) xml::set_attr(schema::ensure_child(tcpr, "w:gridSpan"), "w:val", std::to_string(c2 - c1 + 1));
        if (width) xml::set_attr(schema::ensure_child(tcpr, "w:tcW"), "w:w", std::to_string(width));
        if (r2 > r1) {
            auto vm = schema::ensure_child(tcpr, "w:vMerge");
            if (r == r1) xml::set_attr(vm, "w:val", "restart");
            else {
                xml::remove_attr(vm, "w:val");
                // content of continued cells moves up into the first cell (Word)
                auto top = cell(tbl, r1, c1);
                for (auto p : xml::children(keep, "w:p")) {
                    if (analyze(p).vis.empty()) continue;
                    top.append_move(p);
                }
                if (!keep.child("w:p")) keep.append_child("w:p");
            }
        }
        // remove empty leading paragraphs created by merging (keep one)
        auto ps = xml::children(keep, "w:p");
        if (ps.size() > 1 && analyze(ps[0]).vis.empty()) keep.remove_child(ps[0]);
    }
}

namespace {
void apply_borders(pugi::xml_node container, const Json& v, bool table) {
    // "all" | "none" | {"top":..,"insideH":..}
    static const char* tsides[] = {"top", "left", "bottom", "right", "insideH", "insideV"};
    static const char* csides[] = {"top", "left", "bottom", "right"};
    Json spec = v;
    if (v.is_string() || v.is_boolean()) {
        bool none = v.is_boolean() ? !v.get<bool>() : v.get<std::string>() == "none";
        spec = Json::object();
        const char* const* sides = table ? tsides : csides;
        int n = table ? 6 : 4;
        for (int i = 0; i < n; i++) spec[sides[i]] = none ? Json("nil") : Json("single");
    }
    for (auto& [side, bv] : spec.items()) {
        auto b = schema::ensure_child(container, "w:" + side);
        std::string style = bv.is_string() ? bv.get<std::string>() : bv.value("style", "single");
        xml::set_attr(b, "w:val", style);
        if (style != "nil" && style != "none") {
            double sz = bv.is_object() ? bv.value("size", 0.5) : 0.5;
            xml::set_attr(b, "w:sz", std::to_string(std::lround(sz * 8)));
            xml::set_attr(b, "w:space", "0");
            std::string color = bv.is_object() ? bv.value("color", std::string("auto")) : "auto";
            if (!color.empty() && color[0] == '#') color = color.substr(1);
            xml::set_attr(b, "w:color", color);
        }
    }
}
}  // namespace

void format_table(Document& d, pugi::xml_node tbl, const Json& props, const EditOpts& o) {
    auto tblpr = schema::ensure_child(tbl, "w:tblPr");
    if (tracking(d, o) && !tblpr.child("w:tblPrChange")) {
        pugi::xml_document old;
        auto op = old.append_child("w:tblPr");
        for (auto c : tblpr.children()) if (std::string_view(c.name()) != "w:tblPrChange") op.append_copy(c);
        auto ch = schema::ensure_child(tblpr, "w:tblPrChange");
        stamp(d, ch);
        ch.append_copy(op);
    }
    for (auto& [k, v] : props.items()) {
        if (k == "style") xml::set_attr(schema::ensure_child(tblpr, "w:tblStyle"), "w:val", ensure_style(d, v.get<std::string>()));
        else if (k == "alignment") enum_attr(schema::ensure_child(tblpr, "w:jc"), "w:val", v.get<std::string>());
        else if (k == "width") {
            auto w = schema::ensure_child(tblpr, "w:tblW");
            if (v.is_string() && v.get<std::string>().ends_with("%")) {
                xml::set_attr(w, "w:type", "pct");
                xml::set_attr(w, "w:w", std::to_string(std::lround(std::clamp(util::to_double(v.get<std::string>()), 0.0, 100.0) * 50)));
            } else if (v.is_string() && v.get<std::string>() == "auto") {
                xml::set_attr(w, "w:type", "auto");
                xml::set_attr(w, "w:w", "0");
            } else {
                xml::set_attr(w, "w:type", "dxa");
                xml::set_attr(w, "w:w", std::to_string(twips_of(v)));
            }
        } else if (k == "borders") apply_borders(schema::ensure_child(tblpr, "w:tblBorders"), v, true);
        else if (k == "indent") {
            auto i = schema::ensure_child(tblpr, "w:tblInd");
            xml::set_attr(i, "w:w", std::to_string(twips_of(v)));
            xml::set_attr(i, "w:type", "dxa");
        } else if (k == "layout") enum_attr(schema::ensure_child(tblpr, "w:tblLayout"), "w:type", v.get<std::string>() == "autofit" ? "autofit" : v.get<std::string>());
        else if (k == "cell_margins") {
            auto m = schema::ensure_child(tblpr, "w:tblCellMar");
            auto set = [&](const std::string& side, const Json& sv) {
                // Word writes w:left/w:right for LTR tables; start/end are aliases
                static const std::map<std::string, std::string> sides = {
                    {"top", "top"}, {"bottom", "bottom"}, {"left", "left"}, {"right", "right"},
                    {"start", "left"}, {"end", "right"}};
                auto it = sides.find(util::lower(side));
                if (it == sides.end())
                    throw std::runtime_error(util::unknown_key("cell margin side", side, {"top", "bottom", "left", "right"}));
                auto e = schema::ensure_child(m, "w:" + it->second);
                xml::set_attr(e, "w:w", std::to_string(twips_of(sv)));
                xml::set_attr(e, "w:type", "dxa");
            };
            if (v.is_object()) {
                for (auto& [side, sv] : v.items()) set(side, sv);
            } else {
                for (auto side : {"top", "left", "bottom", "right"}) set(side, v);
            }
        } else if (k == "header_row" || k == "first_column" || k == "last_row" || k == "last_column" || k == "banded_rows" || k == "banded_columns") {
            auto look = schema::ensure_child(tblpr, "w:tblLook");
            static const std::map<std::string, std::pair<const char*, bool>> m = {
                {"header_row", {"w:firstRow", false}}, {"first_column", {"w:firstColumn", false}},
                {"last_row", {"w:lastRow", false}}, {"last_column", {"w:lastColumn", false}},
                {"banded_rows", {"w:noHBand", true}}, {"banded_columns", {"w:noVBand", true}}};
            auto [a, inv] = m.at(k);
            bool on = v.get<bool>();
            xml::set_attr(look, a, (on != inv) ? "1" : "0");
            // keep the legacy hex val consistent
            int val = 0;
            if (xml::attr(look, "w:firstRow") == "1") val |= 0x20;
            if (xml::attr(look, "w:lastRow") == "1") val |= 0x40;
            if (xml::attr(look, "w:firstColumn") == "1") val |= 0x80;
            if (xml::attr(look, "w:lastColumn") == "1") val |= 0x100;
            if (xml::attr(look, "w:noHBand") == "1") val |= 0x200;
            if (xml::attr(look, "w:noVBand") == "1") val |= 0x400;
            char buf[8];
            std::snprintf(buf, sizeof buf, "%04X", val);
            xml::set_attr(look, "w:val", buf);
        } else if (k == "repeat_header") {
            auto tr = tbl.child("w:tr");
            if (v.get<bool>()) schema::ensure_child(schema::ensure_child(tr, "w:trPr"), "w:tblHeader");
            else tr.child("w:trPr").remove_child("w:tblHeader");
        } else if (k == "row_height") {
            for (auto tr : tbl.children("w:tr")) {
                auto h = schema::ensure_child(schema::ensure_child(tr, "w:trPr"), "w:trHeight");
                xml::set_attr(h, "w:val", std::to_string(twips_of(v)));
                xml::set_attr(h, "w:hRule", "atLeast");
            }
        } else if (k == "column_widths") {
            auto grid = tbl.child("w:tblGrid");
            auto cols = xml::children(grid, "w:gridCol");
            for (std::size_t i = 0; i < cols.size() && i < v.size(); i++) xml::set_attr(cols[i], "w:w", std::to_string(twips_of(v[i])));
            for (auto tr : tbl.children("w:tr")) {
                int c = 0;
                for (auto tc : tr.children("w:tc")) {
                    long w = 0;
                    int span = span_of(tc);
                    for (int s = 0; s < span && c + s < (int)cols.size(); s++) w += util::to_long(xml::attr(cols[c + s], "w:w", "0"));
                    xml::set_attr(schema::ensure_child(schema::ensure_child(tc, "w:tcPr"), "w:tcW"), "w:w", std::to_string(w));
                    xml::set_attr(tc.child("w:tcPr").child("w:tcW"), "w:type", "dxa");
                    c += span;
                }
            }
            xml::set_attr(schema::ensure_child(tblpr, "w:tblLayout"), "w:type", "fixed");
        } else {
            throw std::runtime_error(util::unknown_key("table property", k, {"style", "alignment", "width", "indent", "layout", "borders", "cell_margins", "column_widths", "row_height", "header_row", "repeat_header", "first_column", "last_row", "last_column", "banded_rows", "banded_columns"}));
        }
    }
}

void format_cell(Document& d, pugi::xml_node tc, const Json& props, const EditOpts& o) {
    auto tcpr = schema::ensure_child(tc, "w:tcPr");
    bool cell_level = false;
    for (auto& [k, v] : props.items())
        if (k == "shading" || k == "fill" || k == "vertical_align" || k == "width" || k == "borders" ||
            k == "text_direction" || k == "no_wrap")
            cell_level = true;
    if (cell_level && tracking(d, o) && !tcpr.child("w:tcPrChange")) {
        pugi::xml_document old;
        auto op = old.append_child("w:tcPr");
        for (auto c : tcpr.children()) {
            std::string_view q = c.name();
            if (q != "w:tcPrChange" && q != "w:cellIns" && q != "w:cellDel" && q != "w:cellMerge") op.append_copy(c);
        }
        auto ch = schema::ensure_child(tcpr, "w:tcPrChange");
        stamp(d, ch);
        ch.append_copy(op);
    }
    for (auto& [k, v] : props.items()) {
        if (k == "shading" || k == "fill") {
            auto s = schema::ensure_child(tcpr, "w:shd");
            std::string f = v.get<std::string>();
            if (!f.empty() && f[0] == '#') f = f.substr(1);
            xml::set_attr(s, "w:val", "clear");
            xml::set_attr(s, "w:color", "auto");
            xml::set_attr(s, "w:fill", f);
        } else if (k == "vertical_align") enum_attr(schema::ensure_child(tcpr, "w:vAlign"), "w:val", v.get<std::string>());
        else if (k == "width") {
            auto w = schema::ensure_child(tcpr, "w:tcW");
            xml::set_attr(w, "w:w", std::to_string(twips_of(v)));
            xml::set_attr(w, "w:type", "dxa");
        } else if (k == "borders") apply_borders(schema::ensure_child(tcpr, "w:tcBorders"), v, false);
        else if (k == "text_direction") enum_attr(schema::ensure_child(tcpr, "w:textDirection"), "w:val", v.get<std::string>());
        else if (k == "no_wrap") { if (v.get<bool>()) schema::ensure_child(tcpr, "w:noWrap"); else tcpr.remove_child("w:noWrap"); }
        else if (k == "paragraph") { for (auto p : xml::children(tc, "w:p")) format_paragraph(d, p, v, o); }
        else if (k == "run") {
            for (auto p : xml::children(tc, "w:p")) {
                int n = (int)analyze(p).vis.size();
                if (n) format_runs(d, p, 0, n, v, o);
            }
        } else if (is_run_prop(k)) {
            // {"bold": true} on a cell means its text
            for (auto p : xml::children(tc, "w:p")) {
                int n = (int)analyze(p).vis.size();
                if (n) format_runs(d, p, 0, n, Json{{k, v}}, o);
            }
        } else if (is_para_prop(k)) {
            for (auto p : xml::children(tc, "w:p")) format_paragraph(d, p, Json{{k, v}}, o);
        } else throw std::runtime_error(util::unknown_key("cell property", k, {"shading", "fill", 
            "vertical_align", "width", "borders", "text_direction", "no_wrap", "run", "paragraph"}) +
            "; run and paragraph properties like bold or alignment also apply to the cell text");
    }
}

void delete_table(Document& d, pugi::xml_node tbl, const EditOpts& o) {
    if (tracking(d, o)) {
        for (auto tr : tbl.children("w:tr")) mark_row(d, tr, "w:del");
        track_deleted_content(d, tbl);
        return;
    }
    auto parent = tbl.parent();
    parent.remove_child(tbl);
    // the container must still end with a paragraph
    bool has_p = false;
    for (auto c : parent.children()) if (std::string_view(c.name()) == "w:p") has_p = true;
    if (!has_p) parent.insert_child_before("w:p", parent.child("w:sectPr"));
}

Json table_json(pugi::xml_node tbl) {
    Json rows = Json::array();
    for (auto tr : tbl.children("w:tr")) {
        Json row = Json::array();
        for (auto tc : tr.children("w:tc")) {
            std::string t;
            bool first = true;
            for (auto b : blocks(tc)) {
                if (std::string_view(b.name()) != "w:p") { t += first ? "[table]" : "\n[table]"; first = false; continue; }
                t += (first ? "" : "\n") + analyze(b).utf8();
                first = false;
            }
            Json c = t;
            int span = span_of(tc);
            std::string vm = tc.child("w:tcPr").child("w:vMerge") ? xml::attr(tc.child("w:tcPr").child("w:vMerge"), "w:val", "continue") : "";
            if (span > 1 || !vm.empty()) {
                c = Json{{"text", t}};
                if (span > 1) c["span"] = span;
                if (!vm.empty()) c["vmerge"] = vm;
            }
            row.push_back(c);
        }
        rows.push_back(row);
    }
    Json j;
    j["rows"] = rows;
    j["style"] = xml::attr(tbl.child("w:tblPr").child("w:tblStyle"), "w:val");
    j["columns"] = xml::children(tbl.child("w:tblGrid"), "w:gridCol").size();
    return j;
}

}  // namespace docx::wml
