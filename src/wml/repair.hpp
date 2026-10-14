// Repair known faults in documents we save, the way Word fixes them when it
// opens and resaves a file. Each repair only touches markup the schema
// rejects, so a valid document comes out unchanged.
#pragma once
#include <pugixml.hpp>

namespace docx::wml {

class Document;

// Repair one WML part root (document, styles, numbering, settings, ...).
// Returns how many fixes it made.
int repair_part(Document& d, pugi::xml_node root);

}  // namespace docx::wml
