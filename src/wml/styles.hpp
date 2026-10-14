// Styles: lookup, Word built-ins on demand, create/modify, and the
// effective (resolved) formatting of a paragraph or run.
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "wml/document.hpp"

namespace docx::wml {

struct StyleInfo {
    std::string id, name, type, based_on, next, link;
    bool builtin = false, custom = false, in_use = false, hidden = false, quick = false;
    int ui_priority = -1;
};

pugi::xml_node find_style(Document& d, const std::string& id_or_name);
// Ensure a style exists: when missing and it's a Word built-in (by id or
// English name, e.g. "Heading1" / "heading 1"), add Word's definition plus
// the styles it depends on. Returns the styleId. Throws if unknown.
std::string ensure_style(Document& d, const std::string& id_or_name);
std::vector<StyleInfo> list_styles(Document& d, bool include_latent_builtins = false);

// Create or update a style. props: {"name","type":"paragraph|character|table|numbering",
// "based_on","next","link","ui_priority","quick_style",
// "paragraph":{...pPr json}, "run":{...rPr json}}
std::string upsert_style(Document& d, const std::string& id, const nlohmann::json& props);
void delete_style(Document& d, const std::string& id);

// Fully resolved properties (docDefaults -> table style -> numbering ->
// paragraph style chain -> direct), as JSON.
nlohmann::json effective_paragraph(Document& d, pugi::xml_node p);
nlohmann::json effective_run(Document& d, pugi::xml_node p, pugi::xml_node run);

}  // namespace docx::wml
