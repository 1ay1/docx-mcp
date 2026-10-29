// Structural content: tables, pictures, breaks, sections, headers/footers,
// footnotes/endnotes, bookmarks, hyperlinks, fields (TOC, PAGE, ...),
// content controls, lists, equations. All edits honour track changes.
#pragma once
#include <ctime>
#include <optional>
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
// Split Cells: undo a merge at (row, col), or split a plain cell into `cols`
// side by side (0-based row/col; cols >= 1). Returns how many cells result.
int split_cell(Document& d, pugi::xml_node tbl, int row, int col, int cols);
void format_table(Document& d, pugi::xml_node tbl, const Json& props, const EditOpts& o);
// Table style definition (Table Design > New Table Style): whole-table
// props {borders, alignment, indent, cell_margins, shading, band_size} and
// conditional formats {header_row, total_row, first_column, last_column,
// banded_rows, even_rows, banded_columns, ...: {run, paragraph, cell}}.
void apply_table_style(Document& d, pugi::xml_node style, const Json& table, const Json& conditional);
Json table_style_json(pugi::xml_node style);
void format_cell(Document& d, pugi::xml_node tc, const Json& props, const EditOpts& o);
void delete_table(Document& d, pugi::xml_node tbl, const EditOpts& o);
Json table_json(pugi::xml_node tbl);
// Table > Sort by column `col` (0-based). Header rows (repeat_header, or
// `header`) stay put. type: text | number | date. Returns rows sorted.
int sort_table(Document& d, pugi::xml_node tbl, int col, bool descending, const std::string& type, bool header,
               const EditOpts& o);
// Convert Text to Table: one row per paragraph, cells split on sep
// (tab | comma | semicolon | any string). The paragraphs are replaced.
pugi::xml_node text_to_table(Document& d, const std::vector<pugi::xml_node>& ps, const std::string& sep, const Json& opts,
                             const EditOpts& o);
// Convert Table to Text: one paragraph per row, cells joined by sep.
std::vector<pugi::xml_node> table_to_text(Document& d, pugi::xml_node tbl, const std::string& sep, const EditOpts& o);

// ── pictures ──
// Where a picture sits. Inline flows with the text like a character; anything
// else floats (Layout Options): wrap = square | tight | through | top_bottom |
// behind | in_front. Position offsets are in points from `relative_to`
// (horizontal: margin | page | column | character; vertical: margin | page |
// paragraph | line), or an alignment (left/center/right, top/center/bottom).
struct Placement {
    std::string wrap = "inline";
    std::string h_from = "column", v_from = "paragraph";
    std::string h_align, v_align;   // instead of offsets
    double x_pt = 0, y_pt = 0;
    double distance_pt = 9;         // gap between picture and text (Word's 0.13")
};
Placement placement_from_json(const Json& j);
// Insert a picture (PNG/JPEG/GIF/BMP/TIFF/SVG/EMF/WMF bytes) at visible pos
// of p. width/height in points (0 = from image, keep aspect).
pugi::xml_node insert_picture(Document& d, pugi::xml_node p, int pos, const util::Bytes& data, double width_pt,
                              double height_pt, const std::string& alt_text, const std::string& part, const EditOpts& o,
                              const Placement& place = {});
struct PictureInfo { int index; std::string id, name, alt, para, wrap; double width_pt, height_pt; };
std::vector<PictureInfo> list_pictures(Document& d);
// Resize (keeps aspect when one side is 0 and keep_aspect), re-place, or set
// alt text of picture n (1-based, document order).
void set_picture(Document& d, int n, double width_pt, double height_pt, const Json& placement, const Json& alt);
void delete_picture(Document& d, int n, const EditOpts& o);
// Insert > Text Box at visible pos of p: a floating box (wrap square unless
// placement says otherwise) with one paragraph per line of text. style:
// {fill, border (color | {color,size} | false), shape: rect|rounded,
// alignment, format {bold, size...}, auto_fit}. Returns the box's paragraphs.
std::vector<pugi::xml_node> insert_text_box(Document& d, pugi::xml_node p, int pos, const std::string& text, double width_pt,
                                            double height_pt, const Json& style, const EditOpts& o, const Placement& place);

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
// Text watermark behind every page (Design > Watermark). Returns headers touched.
int set_watermark(Document& d, const std::string& text, const Json& opts);
int remove_watermark(Document& d);
// Drop Cap: kind drop | margin | none. Splits the first `letters` into their
// own framed paragraph like Word; none joins them back. Returns the cap paragraph.
pugi::xml_node set_drop_cap(Document& d, pugi::xml_node p, const std::string& kind, int lines, int letters, const EditOpts& o);

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

// Update Fields (F9) with options.
struct PageMap;
struct MergeCursor {
    const Json* rows = nullptr;  // array of objects
    std::size_t row = 0;         // current record (NEXT / NEXTIF advance it)
    bool skip = false;           // SKIPIF fired: drop this record
    std::vector<std::string> missing;  // MERGEFIELD names with no column
};
struct FieldUpdate {
    bool layout = false;        // lay pages out (LibreOffice) for PAGE, NUMPAGES, PAGEREF, index pages
    MergeCursor* merge = nullptr;
    std::string filename;       // the file's path, for FILENAME
    pugi::xml_node only;        // update just this field (its fldChar begin or fldSimple)
};
struct FieldUpdateResult {
    int updated = 0;
    bool pages = false;  // page numbers came from a real layout
};
FieldUpdateResult update_fields(Document& d, const FieldUpdate& u);
// Field results in every story (no TOC/index rebuild). pages may be null.
int evaluate_fields(Document& d, const FieldUpdate& u, const PageMap* pages);
// Replace fields by their results (Ctrl+Shift+F9). kinds: field names
// ("MERGEFIELD", "IF"...), case-insensitive; empty = every field.
int unlink_fields(Document& d, const std::vector<std::string>& kinds);
// Word's date picture (\@ "dddd, MMMM d, yyyy h:mm am/pm").
std::string format_date(const std::tm& t, const std::string& picture);
// Word's numeric picture (\# "$#,##0.00;($#,##0.00)").
std::string format_numeric(double v, const std::string& picture);
// A number in Word's page/list formats (decimal, lowerRoman, upperLetter, ordinal, cardinalText...).
std::string format_number(int n, const std::string& fmt);

// ── mail merge (Mailings) and document variables ──
// One merged document per record: MERGEFIELD / IF / MERGEREC filled in and
// turned into text. single = every record in one document, a section each.
// SKIPIF drops a record; NEXT moves to the next one inside a document.
std::vector<Document> mail_merge(Document& tmpl, const Json& rows, bool single, const std::string& filename,
                                 std::vector<std::string>* notes = nullptr, std::vector<std::size_t>* sources = nullptr);
// Records from a .csv / .tsv (header row first) or .json (array of objects) file.
Json load_merge_data(const std::string& path);
// The MERGEFIELD names a document uses, in order.
std::vector<std::string> merge_field_names(Document& d);
Json document_variables(Document& d);
void set_document_variable(Document& d, const std::string& name, const Json& value);  // null removes
// Mark Entry: a hidden XE field at pos naming an index term (and sub-term).
void mark_index_entry(Document& d, pugi::xml_node p, int pos, const std::string& term, const std::string& sub,
                      const EditOpts& o);
// Insert Index before paragraph `before`: an INDEX field in its own
// continuous section with `columns`, entries built from the XE marks.
// Returns the number of entries.
int insert_index(Document& d, pugi::xml_node before, const Json& opts, const EditOpts& o);

// ── captions and cross-references (References tab) ──
// A "Figure 3: text" caption paragraph (style Caption, SEQ field numbered like
// Word) before or after block `ref` (a paragraph or table). Returns it.
pugi::xml_node insert_caption(Document& d, pugi::xml_node ref, bool after, const std::string& label,
                              const std::string& text, const std::string& separator, const EditOpts& o);
// What a cross-reference can point at: headings, captions, bookmarks, notes.
struct RefTarget { std::string kind, id, text, label, para; };
std::vector<RefTarget> reference_targets(Document& d);
// Insert a cross-reference at visible pos of p. target = paragraph id of a
// heading/caption, a bookmark name, or "footnote:2"/"endnote:1". show:
// text | number | page | label_number | above_below. Adds Word's hidden
// _Ref bookmark around the target when it needs one. Returns the field code.
std::string insert_cross_reference(Document& d, pugi::xml_node p, int pos, const std::string& target,
                                   const std::string& show, bool link, const EditOpts& o);
struct FieldInfo { std::string instr, result, para; };
std::vector<FieldInfo> list_fields(Document& d);

// ── content controls ──
// type: richText|text|checkbox|dropDown|comboBox|date|picture
pugi::xml_node add_content_control(Document& d, pugi::xml_node p, int s, int e, const Json& opts);
struct ControlInfo { std::string id, tag, alias, type, text; bool checked = false; std::string bound; std::vector<std::string> items; };
std::vector<ControlInfo> list_content_controls(Document& d);
void set_content_control(Document& d, const std::string& id_or_tag, const Json& value);
// Bind a content control to a document property (title, author, subject, company, ...)
// or to an element in a custom XML part (xpath into a store created on demand).
// Bound controls show the stored value; setting either side updates both.
void bind_content_control(Document& d, const std::string& id_or_tag, const std::string& property, const std::string& xpath);
// Push stored values into every bound control (after properties or custom XML change).
int refresh_bound_controls(Document& d);
// Write a bound control's value into its store (property or custom XML).
void set_bound_value(Document& d, pugi::xml_node sdt, const std::string& value);
// A repeating section (w15:repeatingSection) around block content, and adding items to one.
pugi::xml_node add_repeating_section(Document& d, pugi::xml_node first, pugi::xml_node last, const std::string& tag);
int add_repeating_item(Document& d, const std::string& id_or_tag, int count);

// ── lists ──
// Make paragraphs a list: kind bullet|number (or a numFmt like lowerLetter).
// Reuses/creates abstractNum definitions the way Word does. Returns numId.
int apply_list(Document& d, const std::vector<pugi::xml_node>& ps, const std::string& kind, int level, const Json& opts,
               const EditOpts& o);
void remove_list(Document& d, const std::vector<pugi::xml_node>& ps, const EditOpts& o);
// The rendered list label of a paragraph ("1.", "a)", "•"), "" if none.
std::string list_label(Document& d, pugi::xml_node p);
void restart_numbering(Document& d, pugi::xml_node p, int start);
// Define New Multilevel List: a list definition from a preset (numbered, legal,
// outline, headings, chapters, bullets, checklist) with per-level overrides:
// {"preset":"outline","levels":[{"format":"upperRoman","text":"%1.","start":1,
//  "style":"Heading1","indent":"0.5in","hanging":"0.25in","after":"space"}, ...]}.
// Levels linked to a style number every paragraph in that style. Returns the num id.
int define_list(Document& d, const Json& spec);
// The document's lists: num id, levels, how many paragraphs use each.
Json list_definitions(Document& d);

// ── citations and bibliography (References) ──
// Sources live in the b:Sources custom XML part Word uses (Manage Sources).
Json list_sources(Document& d);
// src: {type: book|article|website|report|conference|chapter|..., title, authors: ["Last, First" | "First Last" | {last, first}],
// year, publisher, city, journal, volume, issue, pages, url, tag}. Returns the tag.
std::string add_source(Document& d, const Json& src);
bool remove_source(Document& d, const std::string& tag);
// A CITATION field for a source at pos of p (pages: \p page locator).
void insert_citation(Document& d, pugi::xml_node p, int pos, const std::string& tag, const std::string& pages, const EditOpts& o);
// A BIBLIOGRAPHY field after `after`, entries in the document's style. Returns the source count.
int insert_bibliography(Document& d, pugi::xml_node after, const std::string& title, const EditOpts& o);
// Re-render bibliographies and citations, optionally switching style (APA, MLA, Chicago, IEEE).
int rebuild_bibliographies(Document& d, const std::string& style);

// ── charts ──
// Every chart: type, title, series with categories and values (Word's cached data).
Json list_charts(Document& d);
// Insert a chart at pos of p. spec: {"type": column|bar|line|pie|doughnut|area|scatter,
// "title", "categories": [...], "series": [{"name", "values": [...], "color"}], "grouping":
// clustered|stacked|percent, "x_axis_title", "y_axis_title", "legend", "data_labels"}.
pugi::xml_node insert_chart(Document& d, pugi::xml_node p, int pos, const Json& spec, double width_pt, double height_pt,
                            const EditOpts& o);
// Replace a chart's data or settings (1-based); unspecified parts keep what the chart shows.
void set_chart_data(Document& d, int index, const Json& spec);

// ── math ──
// Insert an Office Math equation from a linear-format-ish string ("x^2+y_1=\\frac{a}{b}").
pugi::xml_node insert_equation(Document& d, pugi::xml_node p, int pos, const std::string& linear, bool display,
                               const EditOpts& o);

}  // namespace docx::wml
