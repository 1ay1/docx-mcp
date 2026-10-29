// Markdown to Word paragraphs, lists, tables and styles.
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "wml/document.hpp"
#include "wml/edit.hpp"

namespace docx::wml {

struct MarkdownResult {
    std::vector<pugi::xml_node> paragraphs;
    int tables = 0, lists = 0;
    int skipped_images = 0;  // remote or unreadable images (alt text kept)
    pugi::xml_node last;  // last block written
};

// Write md after (or before) block `ref`.
MarkdownResult insert_markdown(Document& d, pugi::xml_node ref, bool after, const std::string& md, const EditOpts& o);

// Rich content for front ends other than Markdown (HTML): a run of text with
// its formatting (run JSON as format_text takes) and an optional link.
struct RichSpan {
    std::string text;  // \n is a line break
    Json fmt = Json::object();
    std::string link;
};
struct RichBlock {
    enum Kind { Para, Heading, Code, Quote, Item, Table, Rule, PageBreak, Image } kind = Para;
    int level = 0;                    // heading 1-6, list level 0-8
    bool numbered = false;            // list items
    int list = 0;                     // list items with the same id share numbering
    std::vector<RichSpan> spans;
    Json pfmt = Json::object();       // paragraph format (alignment, ...)
    std::string style;                // explicit paragraph style
    std::vector<std::vector<std::vector<RichSpan>>> rows;  // tables: rows of cells of spans
    std::vector<std::vector<Json>> cell_fmt;              // per cell {shading, colspan, header}
    bool header_row = false;
    std::string image;                // data: URI or file path
    double width_pt = 0, height_pt = 0;
    std::string alt;
};
MarkdownResult insert_rich(Document& d, pugi::xml_node ref, bool after, const std::vector<RichBlock>& blocks, const EditOpts& o);

// HTML (as pasted from a browser, an email or a generator) to Word: headings,
// paragraphs, b/i/u/s/sup/sub/code/mark, span styles (color, background,
// font-size, font-family, font-weight), links, ul/ol nesting, blockquote,
// pre, tables (th, colspan, cell background), hr, br, img (data: URIs).
std::vector<RichBlock> parse_html(const std::string& html);

}  // namespace docx::wml
