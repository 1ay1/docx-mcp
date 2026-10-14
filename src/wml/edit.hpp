// Editing operations, all revision-aware: when tracking is on they produce
// exactly the markup Word produces for the same action.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "wml/document.hpp"
#include "wml/text.hpp"

namespace docx::wml {

using Json = nlohmann::json;

struct EditOpts {
    std::optional<bool> track;  // default: document's trackRevisions setting
};

// ── text ──
// Insert text at visible offset `pos` of paragraph p. Inherits the formatting
// of the character before (Word's behaviour), or `rpr` JSON when given.
void insert_text(Document& d, pugi::xml_node p, int pos, const std::u32string& text, const Json& rpr,
                 const EditOpts& o);
// Delete visible range [s, e) of paragraph p.
void delete_text(Document& d, pugi::xml_node p, int s, int e, const EditOpts& o);
// Replace visible range [s, e) with text (delete + insert as one change).
void replace_text(Document& d, pugi::xml_node p, int s, int e, const std::u32string& text, const Json& rpr,
                  const EditOpts& o);

// Find/replace across stories. Returns number of replacements.
struct FindOpts {
    bool match_case = true;
    bool whole_word = false;
    bool regex = false;
    int max = -1;
};
struct Hit {
    pugi::xml_node p;
    std::string part;
    int start, end;
};
std::vector<Hit> find(Document& d, const std::string& needle, const FindOpts& f, bool all_stories = true);
int replace_all(Document& d, const std::string& needle, const std::string& repl, const FindOpts& f,
                const Json& rpr, const EditOpts& o);

// ── paragraphs ──
// Split paragraph at pos (like pressing Enter). Returns the new (second) paragraph.
pugi::xml_node split_paragraph(Document& d, pugi::xml_node p, int pos, const EditOpts& o);
// Insert a new paragraph before/after `ref` with text, copying ref's pPr
// unless style is given. Returns it.
pugi::xml_node insert_paragraph(Document& d, pugi::xml_node ref, bool after, const std::u32string& text,
                                const std::string& style, const Json& rpr, const EditOpts& o);
// Delete a whole paragraph (content + mark).
void delete_paragraph(Document& d, pugi::xml_node p, const EditOpts& o);
// Merge p with the next paragraph (delete p's mark).
void merge_with_next(Document& d, pugi::xml_node p, const EditOpts& o);

// ── formatting ──
// Apply run properties (JSON, e.g. {"bold":true,"color":"FF0000"}) to [s, e).
void format_runs(Document& d, pugi::xml_node p, int s, int e, const Json& props, const EditOpts& o);
// Apply paragraph properties (JSON) to a paragraph.
void format_paragraph(Document& d, pugi::xml_node p, const Json& props, const EditOpts& o);

// Run/paragraph property JSON <-> XML.
void apply_rpr_json(Document& d, pugi::xml_node rpr, const Json& props);
void apply_ppr_json(Document& d, pugi::xml_node ppr, const Json& props);
Json rpr_to_json(pugi::xml_node rpr);
Json ppr_to_json(pugi::xml_node ppr);

// ── revisions ──
struct RevisionInfo {
    std::string id, type, author, date, text, part, para;
    pugi::xml_node node;
};
std::vector<RevisionInfo> list_revisions(Document& d);
struct RevFilter {
    std::vector<std::string> ids;  // empty = all
    std::string author;            // empty = any
};
// Accept or reject revisions. Returns the count handled.
int accept_revisions(Document& d, const RevFilter& f);
int reject_revisions(Document& d, const RevFilter& f);
// Whole-part accept/reject (used by the above and by compare).
int accept_all_in(Document& d, pugi::xml_node root, const RevFilter& f);
int reject_all_in(Document& d, pugi::xml_node root, const RevFilter& f);

// Set an enum attribute to its canonical spelling (case/space-insensitive
// input); throws listing the allowed values when it doesn't match.
std::string enum_attr(pugi::xml_node e, const char* attr, const std::string& value);

// Stamp a revision element with id/author/date.
void stamp(Document& d, pugi::xml_node rev);
// Track a math run as inserted/deleted: wraps the m:r in w:ins/w:del
// (the schema-valid form; Word's own form puts the marker inside m:r).
void wrap_math_run(Document& d, pugi::xml_node mr, const char* q);

}  // namespace docx::wml
