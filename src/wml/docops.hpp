// Document-level features: protection, properties, settings, statistics,
// whole-document read/outline, compare.
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wml/document.hpp"

namespace docx::wml {

using Json = nlohmann::json;

// ── protection (ECMA 17.15.1.29, MS-OI29500 2.1.402) ──
// mode: readOnly|comments|trackedChanges|forms|none. Empty password = no
// password (Word's "Restrict Editing" without one).
void protect(Document& d, const std::string& mode, const std::string& password);
void unprotect(Document& d, const std::string& password);  // throws on a wrong password
Json protection_info(Document& d);
bool check_password(Document& d, const std::string& password);
// Word's hash for a password, given salt/spin (exposed for tests).
std::string protection_hash(const std::string& password, const util::Bytes& salt, int spin, util::HashAlg alg);

// Mark Final / write-protection recommendation ("open as read-only?").
void set_read_only_recommended(Document& d, bool on);

// ── properties ──
Json core_properties(Document& d);
void set_core_properties(Document& d, const Json& props);  // title, subject, creator, keywords, description, category, lastModifiedBy, revision, ...
Json custom_properties(Document& d);
void set_custom_property(Document& d, const std::string& name, const Json& value);  // null removes
Json app_properties(Document& d);

// ── settings ──
Json settings_json(Document& d);
// Supported: track_changes, update_fields_on_open, even_odd_headers,
// default_tab_stop, zoom, hide_spelling_errors, hide_grammar_errors,
// mirror_margins, gutter_at_top, embed_true_type_fonts, do_not_track_moves,
// do_not_track_formatting, compatibility_mode, auto_hyphenation, ...
void set_settings(Document& d, const Json& props);

// ── statistics (Word's "Word Count") ──
Json statistics(Document& d);

// ── reading ──
// Paragraph listing with ids, styles, list labels, revision/comment markers.
Json read_paragraphs(Document& d, int offset, int limit, bool with_runs, const std::string& view);
// Outline: headings tree.
Json outline(Document& d);
// Whole document as Markdown-ish plain text (current or original view).
std::string to_text(Document& d, const std::string& view, bool markdown);

// ── compare (Word's Review > Compare) ──
// Produce a document = `revised` content shown as tracked changes against
// `original`. Paragraph-level LCS, then word-level diff inside matched paragraphs.
Document compare(Document& original, Document& revised, const std::string& author, const Json& opts);

// ── File > Inspect Document ──
// Counts what a document carries beyond its visible text: comments,
// revisions, personal info (authors, last-modified-by, template), hidden
// text, custom XML, custom properties, invisible content (watermarks...).
// With `remove` (a list of those keys, or "all"), cleans them out first and
// reports what was removed.
Json inspect_document(Document& d, const std::vector<std::string>& remove);

}  // namespace docx::wml
