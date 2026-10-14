// XML helpers: namespace normalization, Markup Compatibility, small DOM utils.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

namespace docx::xml {

// Rewrite every element/attribute name to the canonical prefix for its
// namespace URI (w, r, a, w14, ...), map Strict URIs to Transitional, and
// hoist all namespace declarations to the root. Unknown namespaces keep a
// unique prefix. Safe to call on any part.
void normalize(pugi::xml_document& doc);

// Declare a canonical prefix on the root (no-op if present). If ignorable,
// also add it to mc:Ignorable (declaring mc as needed).
void declare(pugi::xml_node root, std::string_view prefix, bool ignorable = false);
// Ensure every prefix used in the tree and every prefix in mc:Ignorable is
// declared on the root; drop Ignorable entries we can't declare.
void fix_declarations(pugi::xml_node root);

// DOM helpers (qualified names, e.g. "w:p").
std::vector<pugi::xml_node> children(pugi::xml_node n, std::string_view qname);
std::vector<pugi::xml_node> descendants(pugi::xml_node n, std::string_view qname);
pugi::xml_node first_descendant(pugi::xml_node n, std::string_view qname);
pugi::xml_node ancestor(pugi::xml_node n, std::string_view qname);
bool is(pugi::xml_node n, std::string_view qname);
// Document element of the tree n lives in.
pugi::xml_node root_of(pugi::xml_node n);
std::string attr(pugi::xml_node n, std::string_view qname, std::string_view def = "");
void set_attr(pugi::xml_node n, std::string_view qname, std::string_view value);
void remove_attr(pugi::xml_node n, std::string_view qname);
// Inner XML of a node (children serialized).
std::string to_string(pugi::xml_node n, bool pretty = false);
// Parse an XML fragment; prefixes resolve against the canonical namespaces.
// Returns the fragment's top-level nodes appended under `holder`.
bool parse_fragment(std::string_view xml, pugi::xml_document& holder, std::string* error = nullptr);
// Deep copy `src` (from any document) as a new child of `parent` before `before` (or appended).
pugi::xml_node copy_into(pugi::xml_node parent, pugi::xml_node src, pugi::xml_node before = {});

}  // namespace docx::xml
