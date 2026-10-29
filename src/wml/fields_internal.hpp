// Shared between fields.cpp (TOC, index) and fieldeval.cpp (field results).
#pragma once
#include <map>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "wml/document.hpp"

namespace docx::wml {

struct PageMap;

namespace detail {

// Settings > updateFields: Word refreshes every field when it opens the file.
void set_update_fields(Document& d);
// Rebuild every TOC's entries from the current headings / captions.
int rebuild_tocs(Document& d);
// Rebuild every INDEX field's entries. pages: "main\x02sub" (lower case) ->
// page labels; null writes placeholders Word replaces on open.
int rebuild_indexes(Document& d, const std::map<std::string, std::vector<std::string>>* pages);
// Instruction text of a field start (w:fldChar begin or w:fldSimple), trimmed.
std::string field_code(pugi::xml_node anchor);
// The matching w:fldChar end of a w:fldChar begin, or null.
pugi::xml_node field_end(pugi::xml_node begin);
// The term of an XE field instruction ("main:sub", escaped colons as \x01), "" if not XE.
std::string xe_term(const std::string& code);
// Document-order traversal inside root.
pugi::xml_node next_node(pugi::xml_node n, pugi::xml_node root);
pugi::xml_node after_subtree(pugi::xml_node n, pugi::xml_node root);
// "one hundred twenty-three"
std::string number_words(int n);
// Outline level of a paragraph, 1-9, 0 for body text.
int outline_level(Document& d, pugi::xml_node p);

}  // namespace detail
}  // namespace docx::wml
