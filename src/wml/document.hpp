// WordprocessingML document: the package plus the Word-level services every
// edit needs (parts, ids, styles, revision authoring).
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "opc/package.hpp"

namespace docx::wml {

struct Author {
    std::string name = "docx-mcp";
    std::string initials;  // empty = derived from name
    std::string date;  // empty = now
};

class Document {
public:
    static Document create();               // Word's blank document
    static Document open(const std::string& path);
    static Document from_bytes(const util::Bytes& b);
    void save(const std::string& path);
    util::Bytes to_bytes();

    opc::Package& pkg() { return pkg_; }
    const std::string& main() const { return main_; }

    pugi::xml_document& doc_xml() { return pkg_.xml(main_); }
    pugi::xml_node body();

    // Part access (created on demand with Word's boilerplate when create=true).
    pugi::xml_node styles(bool create = true);
    pugi::xml_node settings(bool create = true);
    pugi::xml_node numbering(bool create = true);
    pugi::xml_node comments(bool create = true);
    pugi::xml_node comments_ex(bool create = true);
    pugi::xml_node comments_ids(bool create = true);
    pugi::xml_node comments_extensible(bool create = true);
    pugi::xml_node people(bool create = true);
    pugi::xml_node footnotes(bool create = true);
    pugi::xml_node endnotes(bool create = true);
    pugi::xml_node font_table(bool create = true);
    pugi::xml_node core_props(bool create = true);
    pugi::xml_node app_props(bool create = true);
    pugi::xml_node custom_props(bool create = true);

    // Name of a part related to the main part by type ("" if none).
    std::string related(const char* rel_type) const;
    // Add an XML part related from `source` (default main). Returns its name.
    std::string add_related_part(const char* rel_type, const char* content_type, const std::string& stem,
                                 const std::string& xml, const std::string& source = "");

    // Every story part that holds WordprocessingML content (main, headers,
    // footers, footnotes, endnotes, comments, glossary).
    std::vector<std::string> story_parts();

    // ── ids ──
    // Next free annotation id (w:id on ins/del/comments/bookmarks...). Word
    // keeps one counter across revision and comment annotations.
    int next_annotation_id();
    int next_bookmark_id();
    // A fresh 8-hex-digit id below 0x80000000, unique among w14:paraId/textId
    // and w15/w16cid ids in the package.
    std::string new_hex_id();
    // Give every w:p in a story a w14:paraId/textId if missing.
    void ensure_para_ids(pugi::xml_node root);

    // ── authoring ──
    Author author;
    std::string date();  // author.date or now
    bool track_changes() ;  // settings/w:trackRevisions
    void set_track_changes(bool on);
    // The person entry in people.xml (added if missing).
    void ensure_person(const std::string& name);

    // Rebuild declarations/mc:Ignorable on a part before save.
    void touch(const std::string& part);

    // Run every pending fix-up (ids, declarations, ordering) — called by save.
    void finalize();

private:
    opc::Package pkg_;
    std::string main_;
    std::set<std::string> hex_ids_;
    bool hex_scanned_ = false;
    int max_ann_id_ = -1;
    int max_bm_id_ = -1;
    bool ids_scanned_ = false;
    void scan_ids();
    void scan_hex_ids();
    void dedupe_ids();
    pugi::xml_node part_root(const char* rel_type, const char* ctype, const std::string& stem,
                             const char* root_xml, bool create);
};

// Canonical namespace declarations Word writes on WML part roots.
std::string wml_root_open(const char* qname);

}  // namespace docx::wml
