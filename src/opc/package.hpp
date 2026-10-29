// OPC package (ECMA-376 Part 2): parts, content types, relationships.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pugixml.hpp>

#include "util/util.hpp"

namespace docx::opc {

namespace rt {  // relationship types
inline constexpr const char* officeDocument = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
inline constexpr const char* coreProps = "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties";
inline constexpr const char* extendedProps = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties";
inline constexpr const char* customProps = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/custom-properties";
inline constexpr const char* thumbnail = "http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail";
inline constexpr const char* styles = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";
inline constexpr const char* stylesWithEffects = "http://schemas.microsoft.com/office/2007/relationships/stylesWithEffects";
inline constexpr const char* settings = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings";
inline constexpr const char* webSettings = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/webSettings";
inline constexpr const char* fontTable = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/fontTable";
inline constexpr const char* theme = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme";
inline constexpr const char* numbering = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering";
inline constexpr const char* comments = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments";
inline constexpr const char* commentsExtended = "http://schemas.microsoft.com/office/2011/relationships/commentsExtended";
inline constexpr const char* commentsIds = "http://schemas.microsoft.com/office/2016/09/relationships/commentsIds";
inline constexpr const char* commentsExtensible = "http://schemas.microsoft.com/office/2018/08/relationships/commentsExtensible";
inline constexpr const char* people = "http://schemas.microsoft.com/office/2011/relationships/people";
inline constexpr const char* footnotes = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes";
inline constexpr const char* endnotes = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/endnotes";
inline constexpr const char* header = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/header";
inline constexpr const char* footer = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer";
inline constexpr const char* image = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
inline constexpr const char* hyperlink = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
inline constexpr const char* glossaryDocument = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/glossaryDocument";
inline constexpr const char* customXml = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXml";
inline constexpr const char* customXmlProps = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXmlProps";
inline constexpr const char* chart = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart";
inline constexpr const char* attachedTemplate = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate";
}  // namespace rt

namespace ct {  // content types
inline constexpr const char* rels = "application/vnd.openxmlformats-package.relationships+xml";
inline constexpr const char* xml = "application/xml";
inline constexpr const char* document = "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
inline constexpr const char* documentMacro = "application/vnd.ms-word.document.macroEnabled.main+xml";
inline constexpr const char* templateMain = "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml";
inline constexpr const char* templateMacro = "application/vnd.ms-word.template.macroEnabledTemplate.main+xml";
inline constexpr const char* styles = "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml";
inline constexpr const char* settings = "application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml";
inline constexpr const char* webSettings = "application/vnd.openxmlformats-officedocument.wordprocessingml.webSettings+xml";
inline constexpr const char* fontTable = "application/vnd.openxmlformats-officedocument.wordprocessingml.fontTable+xml";
inline constexpr const char* theme = "application/vnd.openxmlformats-officedocument.theme+xml";
inline constexpr const char* numbering = "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml";
inline constexpr const char* comments = "application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml";
inline constexpr const char* commentsExtended = "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtended+xml";
inline constexpr const char* commentsIds = "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsIds+xml";
inline constexpr const char* commentsExtensible = "application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtensible+xml";
inline constexpr const char* people = "application/vnd.openxmlformats-officedocument.wordprocessingml.people+xml";
inline constexpr const char* footnotes = "application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml";
inline constexpr const char* endnotes = "application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml";
inline constexpr const char* header = "application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml";
inline constexpr const char* footer = "application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml";
inline constexpr const char* coreProps = "application/vnd.openxmlformats-package.core-properties+xml";
inline constexpr const char* extendedProps = "application/vnd.openxmlformats-officedocument.extended-properties+xml";
inline constexpr const char* customProps = "application/vnd.openxmlformats-officedocument.custom-properties+xml";
inline constexpr const char* customXmlProps = "application/vnd.openxmlformats-officedocument.customXmlProperties+xml";
}  // namespace ct

enum class TargetMode { Internal, External };

struct Relationship {
    std::string id;
    std::string type;
    std::string target;  // as written (relative for internal)
    TargetMode mode = TargetMode::Internal;
};

class Package;

// Relationships of one source (a part, or the package itself with source "/").
class Rels {
public:
    std::vector<Relationship> items;

    const Relationship* by_id(std::string_view id) const;
    std::vector<const Relationship*> by_type(std::string_view type) const;
    const Relationship* first_of(std::string_view type) const;
    // Adds a relationship and returns its id (rIdN, unused).
    std::string add(std::string type, std::string target, TargetMode mode = TargetMode::Internal);
    bool remove(std::string_view id);
    std::string next_id() const;
};

struct Part {
    std::string name;          // absolute part name, e.g. "/word/document.xml"
    std::string content_type;
    util::Bytes bytes;         // raw content when not parsed
    std::unique_ptr<pugi::xml_document> xml;  // parsed on demand
    bool dirty = false;
};

class Package {
public:
    static Package open(const std::string& path);
    static Package from_bytes(const util::Bytes& zip);
    void save(const std::string& path) const;
    // level: zlib level; 0 stores (fast, for in-memory snapshots)
    util::Bytes to_bytes(int level = 6) const;

    bool has(std::string_view name) const;
    Part* part(std::string_view name);
    const Part* part(std::string_view name) const;
    // XML of a part, parsed on first use. Throws if missing or malformed.
    pugi::xml_document& xml(std::string_view name);
    Part& add_part(std::string name, std::string content_type, util::Bytes bytes = {});
    Part& add_xml_part(std::string name, std::string content_type, std::string_view xml_text);
    void remove_part(std::string_view name);
    std::vector<std::string> part_names() const;

    // Relationships for a source part ("/" = package).
    Rels& rels(std::string_view source);
    const Rels* rels_if(std::string_view source) const;

    // Resolve a relationship target against its source part.
    static std::string resolve(std::string_view source, std::string_view target);
    // Relative target from source to an absolute part name.
    static std::string relative(std::string_view source, std::string_view part_name);

    // Main document part ("/word/document.xml" usually).
    std::string main_part() const;
    // Target part of the first rel of a type from a source, or "".
    std::string related(std::string_view source, std::string_view type) const;
    // A part name not in use, e.g. unique_name("/word/header", ".xml") -> /word/header3.xml
    std::string unique_name(std::string_view stem, std::string_view ext) const;

    std::string content_type_of(std::string_view name) const;
    void set_override(std::string name, std::string content_type);

    std::map<std::string, std::string> defaults;  // extension (lower) -> content type
    // Applied to XML parts added through add_xml_part (namespace normalization).
    std::function<void(pugi::xml_document&)> normalizer;

private:
    std::map<std::string, Part> parts_;  // key: lower-case part name
    std::map<std::string, Rels> rels_;   // key: lower-case source part name
    std::vector<std::string> order_;     // original part order (lower-case keys)
};

std::string key_of(std::string_view name);
std::string xml_to_string(const pugi::xml_document& d);

}  // namespace docx::opc
