// Schema runtime over the generated Open XML SDK tables: element type
// lookup, canonical child order, and a validator for the Microsoft365 format.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

namespace docx::schema {

using TypeId = std::int32_t;
inline constexpr TypeId NoType = -1;

enum class ValueKind : std::uint8_t {
    String, Boolean, OnOff, TrueFalse, TrueFalseBlank, Byte, SByte, Int16, UInt16, Int32, UInt32,
    Int64, UInt64, Integer, Decimal, Double, Single, HexBinary, Base64Binary, DateTime, Enum, List
};

struct AttrInfo {
    std::string qname;
    ValueKind kind;
    std::vector<std::string> enum_values;  // when kind is Enum (or list of enums)
    bool required = false;
};

struct ChildInfo {
    std::string qname;
    TypeId type;
    std::uint32_t min, max;  // 0 max = unbounded (as seen at this position)
};

// Namespaces
std::string_view uri_of_prefix(std::string_view prefix);   // "" if unknown
std::string_view prefix_of_uri(std::string_view uri);      // "" if unknown
bool namespace_known(std::string_view uri);

// Types
TypeId root_type(std::string_view qname);  // for a part root element, e.g. "w:document"
// Root type of a part by content type, NoType if the SDK doesn't type it.
TypeId part_root_type(std::string_view content_type);
TypeId child_type(TypeId parent, std::string_view qname);
std::string_view type_key(TypeId t);       // "w:CT_PPr/w:pPr"
std::string_view type_qname(TypeId t);
std::string_view type_summary(TypeId t);
bool is_leaf_text(TypeId t);
std::vector<ChildInfo> children_of(TypeId t);  // schema order
std::vector<AttrInfo> attributes_of(TypeId t);

// Rank of a child in its parent's content model (for ordering). -1 if the
// child isn't in the model.
int child_rank(TypeId parent, std::string_view qname);

// Type of a node, resolved by walking up to the part root.
TypeId type_of(pugi::xml_node n);

// Get or create child `qname` of `parent` at its schema position.
pugi::xml_node ensure_child(pugi::xml_node parent, std::string_view qname);
// Insert a new child `qname` at its schema position (even if one exists).
pugi::xml_node insert_child(pugi::xml_node parent, std::string_view qname);
// Move an existing child to its schema position.
void place_child(pugi::xml_node parent, pugi::xml_node child);
// Stable-sort an element's children into schema order (unknown children
// stay attached to their predecessor).
void sort_children(pugi::xml_node parent);

struct Issue {
    std::string path;     // xpath-ish
    std::string message;
};

struct ValidateOptions {
    std::size_t max_issues = 200;
    bool semantic = true;  // the SDK's semantic constraints (unique ids, ranges, ...)
};

// Validate a part's XML against the schema. root must be the document element.
std::vector<Issue> validate(pugi::xml_node root, const ValidateOptions& opt = {});

// Validate one attribute value against its declared type.
bool value_valid(TypeId t, std::string_view attr_qname, std::string_view value, std::string* why = nullptr);

}  // namespace docx::schema
