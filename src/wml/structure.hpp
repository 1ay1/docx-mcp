// Structural content: tables, pictures, breaks, sections, headers/footers,
// footnotes/endnotes, bookmarks, hyperlinks, fields (TOC, PAGE, ...),
// content controls, lists, equations. All edits honour track changes.
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "wml/document.hpp"
#include "wml/edit.hpp"

namespace docx::wml {

using Json = nlohmann::json;

// ── tables ──
// Insert a table after (or before) block `ref`. rows: [[cell text...]...].
// opts: {"style":"TableGrid","header_row":true,"widths":[pt...],"autofit":true,
//        "borders":..., "alignment":"center"}
pugi::xml_node insert_table(Document& d, pugi::xml_node ref, bool after, const std::vector<std::vector<std::string>>& rows,
                            const Json& opts, const EditOpts& o);
pugi::xml_node table_at(Document& d, int index);  // 1-based over body tables
std::vector<pugi::xml_node> tables(Document& d);
pugi::xml_node cell(pugi::xml_node tbl, int row, int col);  // 0-based, accounts for gridSpan
void set_cell_text(Document& d, pugi::xml_node tc, const std::string& text, const EditOpts& o);
pugi::xml_node insert_row(Document& d, pugi::xml_node tbl, int at, bool after, const std::vector<std::string>& cells, const EditOpts& o);
void delete_row(Document& d, pugi::xml_node tbl, int row, const EditOpts& o);
void insert_column(Document& d, pugi::xml_node tbl, int at, bool after, const EditOpts& o);
void delete_column(Document& d, pugi::xml_node tbl, int col, const EditOpts& o);
void merge_cells(Document& d, pugi::xml_node tbl, int r1, int c1, int r2, int c2);
void format_table(Document& d, pugi::xml_node tbl, const Json& props, const EditOpts& o);
void format_cell(Document& d, pugi::xml_node tc, const Json& props, const EditOpts& o);
void delete_table(Document& d, pugi::xml_node tbl, const EditOpts& o);
Json table_json(pugi::xml_node tbl);

// ── pictures ──
// Insert an inline picture (PNG/JPEG/GIF/BMP/TIFF/SVG/EMF/WMF bytes) at
// visible pos of p. width/height in points (0 = from image, keep aspect).
pugi::xml_node insert_picture(Document& d, pugi::xml_node p, int pos, const util::Bytes& data, double width_pt,
                              double height_pt, const std::string& alt_text, const std::string& part, const EditOpts& o);

// ── breaks ──
void insert_break(Document& d, pugi::xml_node p, int pos, const std::string& type, const EditOpts& o);  // page|column|line
// Section break after paragraph p: type nextPage|continuous|evenPage|oddPage|nextColumn.
void insert_section_break(Document& d, pugi::xml_node p, const std::string& type, const EditOpts& o);
std::vector<pugi::xml_node> sections(Document& d);  // sectPr elements in order
// props: {"orientation","page_width","page_height","margins":{top,bottom,left,right,header,footer,gutter},
//         "columns":n,"column_space","type","title_page","line_numbers":{...},"page_numbers":{"start":1,"format":"lowerRoman"}}
void format_section(Document& d, int index, const Json& props, const EditOpts& o);
Json section_json(Document& d, pugi::xml_node sectPr);

// ── headers / footers ──
// kind: header|footer, which: default|first|even. Creates the part when
// missing. Returns the part name.
std::string header_footer(Document& d, int section, const std::string& kind, const std::string& which, bool create);
void set_header_footer_text(Document& d, int section, const std::string& kind, const std::string& which,
                            const std::string& text, const Json& opts);
void remove_header_footer(Document& d, int section, const std::string& kind, const std::string& which);

// ── notes ──
std::string add_note(Document& d, pugi::xml_node p, int pos, const std::string& kind, const std::string& text, const EditOpts& o);
struct NoteInfo { std::string id, kind, text, anchor_para; };
std::vector<NoteInfo> list_notes(Document& d);
void delete_note(Document& d, const std::string& kind, const std::string& id, const EditOpts& o);

// ── bookmarks ──
void add_bookmark(Document& d, pugi::xml_node p1, int s, pugi::xml_node p2, int e, const std::string& name);
void delete_bookmark(Document& d, const std::string& name);
struct BookmarkInfo { std::string name, id, text, para; };
std::vector<BookmarkInfo> list_bookmarks(Document& d);

// ── hyperlinks ──
// target: URL, or "#bookmark" for an internal link. Wraps [s,e) or inserts text at s.
void add_hyperlink(Document& d, pugi::xml_node p, int s, int e, const std::string& target, const std::string& text,
                   const std::string& tooltip, const EditOpts& o);
struct LinkInfo { std::string text, target, para; };
std::vector<LinkInfo> list_hyperlinks(Document& d);
int remove_hyperlinks(Document& d, const std::string& target_substring);

// ── fields ──
// Insert a complex field (e.g. "PAGE", "NUMPAGES", "DATE \\@ \"d MMMM yyyy\"",
// "REF bm \\h", "SEQ Figure \\* ARABIC") with a cached result.
void insert_field(Document& d, pugi::xml_node p, int pos, const std::string& instr, const std::string& result,
                  const Json& rpr, const EditOpts& o);
// Insert a table of contents before paragraph p (Word's docPartObj SDT with
// TOC field, entries built from the current headings).
void insert_toc(Document& d, pugi::xml_node p, const Json& opts, const EditOpts& o);
// Recompute TOC entries, SEQ/REF/STYLEREF results that don't need layout.
// Sets updateFields so Word refreshes page numbers on open.
int update_fields(Document& d);
struct FieldInfo { std::string instr, result, para; };
std::vector<FieldInfo> list_fields(Document& d);

// ── content controls ──
// type: richText|text|checkbox|dropDown|comboBox|date|picture
pugi::xml_node add_content_control(Document& d, pugi::xml_node p, int s, int e, const Json& opts);
struct ControlInfo { std::string id, tag, alias, type, text; bool checked = false; };
std::vector<ControlInfo> list_content_controls(Document& d);
void set_content_control(Document& d, const std::string& id_or_tag, const Json& value);

// ── lists ──
// Make paragraphs a list: kind bullet|number (or a numFmt like lowerLetter).
// Reuses/creates abstractNum definitions the way Word does. Returns numId.
int apply_list(Document& d, const std::vector<pugi::xml_node>& ps, const std::string& kind, int level, const Json& opts,
               const EditOpts& o);
void remove_list(Document& d, const std::vector<pugi::xml_node>& ps, const EditOpts& o);
// The rendered list label of a paragraph ("1.", "a)", "•"), "" if none.
std::string list_label(Document& d, pugi::xml_node p);
void restart_numbering(Document& d, pugi::xml_node p, int start);

// ── math ──
// Insert an Office Math equation from a linear-format-ish string ("x^2+y_1=\\frac{a}{b}").
pugi::xml_node insert_equation(Document& d, pugi::xml_node p, int pos, const std::string& linear, bool display,
                               const EditOpts& o);

}  // namespace docx::wml
