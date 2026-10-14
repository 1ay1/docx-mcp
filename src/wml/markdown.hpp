// Markdown to Word paragraphs, lists, tables and styles.
#pragma once
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "wml/document.hpp"
#include "wml/edit.hpp"

namespace docx::wml {

struct MarkdownResult {
    std::vector<pugi::xml_node> paragraphs;
    int tables = 0, lists = 0;
    pugi::xml_node last;  // last block written
};

// Write md after (or before) block `ref`.
MarkdownResult insert_markdown(Document& d, pugi::xml_node ref, bool after, const std::string& md, const EditOpts& o);

}  // namespace docx::wml
