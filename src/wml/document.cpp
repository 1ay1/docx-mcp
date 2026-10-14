#include "wml/document.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/assets.hpp"
#include "util/util.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

// The namespaces Word 365 declares on every WML part root, and the
// mc:Ignorable list it writes (order as Word writes it).
const char* const kRootPrefixes[] = {"wpc", "cx", "cx1", "cx2", "cx3", "cx4", "cx5", "cx6", "cx7", "cx8",
                                     "mc", "aink", "am3d", "o", "oel", "r", "m", "v", "wp14", "wp", "w10",
                                     "w", "w14", "w15", "w16cex", "w16cid", "w16", "w16du", "w16sdtdh",
                                     "w16sdtfl", "w16se", "wpg", "wpi", "wne", "wps"};
// Prefixes the SDK data doesn't list but Word declares.
const std::map<std::string, std::string>& extra_ns() {
    static const std::map<std::string, std::string> m = {
        {"cx1", "http://schemas.microsoft.com/office/drawing/2015/9/8/chartex"},
        {"cx2", "http://schemas.microsoft.com/office/drawing/2015/10/21/chartex"},
        {"cx3", "http://schemas.microsoft.com/office/drawing/2016/5/9/chartex"},
        {"cx4", "http://schemas.microsoft.com/office/drawing/2016/5/10/chartex"},
        {"cx5", "http://schemas.microsoft.com/office/drawing/2016/5/11/chartex"},
        {"cx6", "http://schemas.microsoft.com/office/drawing/2016/5/12/chartex"},
        {"cx7", "http://schemas.microsoft.com/office/drawing/2016/5/13/chartex"},
        {"cx8", "http://schemas.microsoft.com/office/drawing/2016/5/14/chartex"},
        {"w16", "http://schemas.microsoft.com/office/word/2018/wordml"},
        {"mc", "http://schemas.openxmlformats.org/markup-compatibility/2006"},
    };
    return m;
}
const char* kIgnorable = "w14 w15 w16se w16cid w16 w16cex w16sdtdh w16sdtfl w16du wp14";

std::string uri(const char* p) {
    auto& ex = extra_ns();
    if (auto it = ex.find(p); it != ex.end()) return it->second;
    return std::string(schema::uri_of_prefix(p));
}

constexpr const char* kXmlDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

std::string blank_document_xml() {
    return std::string(kXmlDecl) + wml_root_open("w:document") +
           "<w:body><w:p/><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
           "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
           "w:footer=\"720\" w:gutter=\"0\"/><w:cols w:space=\"720\"/><w:docGrid w:linePitch=\"360\"/>"
           "</w:sectPr></w:body></w:document>";
}

}  // namespace

std::string wml_root_open(const char* qname) {
    std::string s = "<";
    s += qname;
    for (auto p : kRootPrefixes) s += " xmlns:" + std::string(p) + "=\"" + uri(p) + "\"";
    s += " mc:Ignorable=\"" + std::string(kIgnorable) + "\">";
    return s;
}

Document Document::create() {
    Document d;
    auto& p = d.pkg_;
    p.defaults["rels"] = opc::ct::rels;
    p.defaults["xml"] = opc::ct::xml;
    d.main_ = "/word/document.xml";
    p.add_xml_part(d.main_, opc::ct::document, blank_document_xml());
    p.rels("/").add(opc::rt::officeDocument, "word/document.xml");
    auto add = [&](const char* name, const char* ctype, const char* rtype, const char* asset) {
        p.add_part(name, ctype, util::Bytes(assets::get(asset).begin(), assets::get(asset).end()));
        p.rels(d.main_).add(rtype, opc::Package::relative(d.main_, name));
    };
    add("/word/styles.xml", opc::ct::styles, opc::rt::styles, "word/blank/styles.xml");
    add("/word/settings.xml", opc::ct::settings, opc::rt::settings, "word/blank/settings.xml");
    add("/word/webSettings.xml", opc::ct::webSettings, opc::rt::webSettings, "word/blank/webSettings.xml");
    add("/word/fontTable.xml", opc::ct::fontTable, opc::rt::fontTable, "word/blank/fontTable.xml");
    add("/word/theme/theme1.xml", opc::ct::theme, opc::rt::theme, "word/blank/theme1.xml");
    d.core_props(true);
    d.app_props(true);
    // Word gives every new document fresh ids.
    auto st = d.settings();
    auto d14 = schema::ensure_child(st, "w14:docId");
    xml::set_attr(d14, "w14:val", d.new_hex_id());
    auto d15 = schema::ensure_child(st, "w15:docId");
    xml::set_attr(d15, "w15:val", util::new_guid());
    return d;
}

Document Document::from_bytes(const util::Bytes& b) {
    Document d;
    d.pkg_ = opc::Package::from_bytes(b);
    d.main_ = d.pkg_.main_part();
    if (d.main_.empty() || !d.pkg_.has(d.main_)) throw std::runtime_error("package has no main document part");
    std::string ct = d.pkg_.content_type_of(d.main_);
    if (ct.find("wordprocessingml") == std::string::npos && ct.find("ms-word") == std::string::npos)
        throw std::runtime_error("not a Word document (main part is " + ct + ")");
    // Normalize every WML part we may touch so prefixes are canonical.
    for (auto& name : d.pkg_.part_names()) {
        std::string pct = d.pkg_.content_type_of(name);
        if (schema::part_root_type(pct) < 0) continue;
        auto& x = d.pkg_.xml(name);
        xml::normalize(x);
    }
    return d;
}

Document Document::open(const std::string& path) {
    auto pkg = opc::Package::open(path);
    return from_bytes(pkg.to_bytes());
}

util::Bytes Document::to_bytes() {
    finalize();
    return pkg_.to_bytes();
}

void Document::save(const std::string& path) {
    finalize();
    pkg_.save(path);
}

pugi::xml_node Document::body() {
    auto root = doc_xml().document_element();
    auto b = root.child("w:body");
    if (!b) b = schema::ensure_child(root, "w:body");
    return b;
}

std::string Document::related(const char* rel_type) const { return pkg_.related(main_, rel_type); }

std::string Document::add_related_part(const char* rel_type, const char* ctype, const std::string& stem,
                                       const std::string& xmltext, const std::string& source) {
    std::string src = source.empty() ? main_ : source;
    std::string name = pkg_.has(stem + ".xml") ? pkg_.unique_name(stem, ".xml") : stem + ".xml";
    pkg_.add_xml_part(name, ctype, xmltext);
    pkg_.rels(src).add(rel_type, opc::Package::relative(src, name));
    return name;
}

pugi::xml_node Document::part_root(const char* rel_type, const char* ctype, const std::string& stem,
                                   const char* root_qname, bool create) {
    std::string name = related(rel_type);
    if (name.empty() || !pkg_.has(name)) {
        if (!create) return {};
        std::string x = std::string(kXmlDecl) + wml_root_open(root_qname) + "</" + root_qname + ">";
        name = add_related_part(rel_type, ctype, stem, x);
    }
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::styles(bool create) {
    std::string name = related(opc::rt::styles);
    if (name.empty() || !pkg_.has(name)) {
        if (!create) return {};
        auto s = assets::get("word/blank/styles.xml");
        name = add_related_part(opc::rt::styles, opc::ct::styles, "/word/styles", std::string(s));
    }
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::settings(bool create) {
    std::string name = related(opc::rt::settings);
    if (name.empty() || !pkg_.has(name)) {
        if (!create) return {};
        auto s = assets::get("word/blank/settings.xml");
        name = add_related_part(opc::rt::settings, opc::ct::settings, "/word/settings", std::string(s));
    }
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::numbering(bool c) {
    return part_root(opc::rt::numbering, opc::ct::numbering, "/word/numbering", "w:numbering", c);
}
pugi::xml_node Document::comments(bool c) {
    return part_root(opc::rt::comments, opc::ct::comments, "/word/comments", "w:comments", c);
}
pugi::xml_node Document::comments_ex(bool c) {
    return part_root(opc::rt::commentsExtended, opc::ct::commentsExtended, "/word/commentsExtended", "w15:commentsEx", c);
}
pugi::xml_node Document::comments_ids(bool c) {
    return part_root(opc::rt::commentsIds, opc::ct::commentsIds, "/word/commentsIds", "w16cid:commentsIds", c);
}
pugi::xml_node Document::comments_extensible(bool c) {
    return part_root(opc::rt::commentsExtensible, opc::ct::commentsExtensible, "/word/commentsExtensible",
                     "w16cex:commentsExtensible", c);
}
pugi::xml_node Document::people(bool c) {
    return part_root(opc::rt::people, opc::ct::people, "/word/people", "w15:people", c);
}
pugi::xml_node Document::font_table(bool c) {
    return part_root(opc::rt::fontTable, opc::ct::fontTable, "/word/fontTable", "w:fonts", c);
}

namespace {
const char* kSeparatorNotes(const char* kind) {
    // Word always writes the separator (-1) and continuation separator (0) notes.
    static std::string fn, en;
    std::string& s = std::string(kind) == "footnote" ? fn : en;
    if (s.empty()) {
        std::string tag = std::string("w:") + kind;
        s = "<" + tag + " w:type=\"separator\" w:id=\"-1\"><w:p><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" "
            "w:lineRule=\"auto\"/></w:pPr><w:r><w:separator/></w:r></w:p></" + tag + ">" +
            "<" + tag + " w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:pPr><w:spacing w:after=\"0\" "
            "w:line=\"240\" w:lineRule=\"auto\"/></w:pPr><w:r><w:continuationSeparator/></w:r></w:p></" + tag + ">";
    }
    return s.c_str();
}
}  // namespace

pugi::xml_node Document::footnotes(bool c) {
    std::string name = related(opc::rt::footnotes);
    if ((name.empty() || !pkg_.has(name)) && c) {
        std::string x = std::string(kXmlDecl) + wml_root_open("w:footnotes") + kSeparatorNotes("footnote") + "</w:footnotes>";
        name = add_related_part(opc::rt::footnotes, opc::ct::footnotes, "/word/footnotes", x);
        auto st = settings();
        auto fp = schema::ensure_child(st, "w:footnotePr");
        if (!fp.child("w:footnote")) {
            xml::set_attr(schema::insert_child(fp, "w:footnote"), "w:id", "-1");
            xml::set_attr(schema::insert_child(fp, "w:footnote"), "w:id", "0");
        }
    }
    if (name.empty()) return {};
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::endnotes(bool c) {
    std::string name = related(opc::rt::endnotes);
    if ((name.empty() || !pkg_.has(name)) && c) {
        std::string x = std::string(kXmlDecl) + wml_root_open("w:endnotes") + kSeparatorNotes("endnote") + "</w:endnotes>";
        name = add_related_part(opc::rt::endnotes, opc::ct::endnotes, "/word/endnotes", x);
        auto st = settings();
        auto ep = schema::ensure_child(st, "w:endnotePr");
        if (!ep.child("w:endnote")) {
            xml::set_attr(schema::insert_child(ep, "w:endnote"), "w:id", "-1");
            xml::set_attr(schema::insert_child(ep, "w:endnote"), "w:id", "0");
        }
    }
    if (name.empty()) return {};
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::core_props(bool c) {
    std::string name = pkg_.related("/", opc::rt::coreProps);
    if ((name.empty() || !pkg_.has(name)) && c) {
        std::string now = util::now_iso8601();
        std::string x = std::string(kXmlDecl) +
            "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
            "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<dc:title></dc:title><dc:subject></dc:subject><dc:creator>" + author.name + "</dc:creator>"
            "<cp:keywords></cp:keywords><dc:description></dc:description><cp:lastModifiedBy>" + author.name +
            "</cp:lastModifiedBy><cp:revision>1</cp:revision>"
            "<dcterms:created xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:created>"
            "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:modified></cp:coreProperties>";
        name = "/docProps/core.xml";
        pkg_.add_xml_part(name, opc::ct::coreProps, x);
        pkg_.rels("/").add(opc::rt::coreProps, "docProps/core.xml");
    }
    if (name.empty()) return {};
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::app_props(bool c) {
    std::string name = pkg_.related("/", opc::rt::extendedProps);
    if ((name.empty() || !pkg_.has(name)) && c) {
        std::string x = std::string(kXmlDecl) +
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
            "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
            "<Template>Normal.dotm</Template><TotalTime>0</TotalTime><Pages>1</Pages><Words>0</Words>"
            "<Characters>0</Characters><Application>Microsoft Office Word</Application><DocSecurity>0</DocSecurity>"
            "<Lines>0</Lines><Paragraphs>0</Paragraphs><ScaleCrop>false</ScaleCrop><Company></Company>"
            "<LinksUpToDate>false</LinksUpToDate><CharactersWithSpaces>0</CharactersWithSpaces>"
            "<SharedDoc>false</SharedDoc><HyperlinksChanged>false</HyperlinksChanged><AppVersion>16.0000</AppVersion>"
            "</Properties>";
        name = "/docProps/app.xml";
        pkg_.add_xml_part(name, opc::ct::extendedProps, x);
        pkg_.rels("/").add(opc::rt::extendedProps, "docProps/app.xml");
    }
    if (name.empty()) return {};
    return pkg_.xml(name).document_element();
}

pugi::xml_node Document::custom_props(bool c) {
    std::string name = pkg_.related("/", opc::rt::customProps);
    if ((name.empty() || !pkg_.has(name)) && c) {
        std::string x = std::string(kXmlDecl) +
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\" "
            "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"></Properties>";
        name = "/docProps/custom.xml";
        pkg_.add_xml_part(name, opc::ct::customProps, x);
        pkg_.rels("/").add(opc::rt::customProps, "docProps/custom.xml");
    }
    if (name.empty()) return {};
    return pkg_.xml(name).document_element();
}

std::vector<std::string> Document::story_parts() {
    std::vector<std::string> v{main_};
    if (const auto* r = pkg_.rels_if(main_)) {
        for (auto& rel : r->items) {
            if (rel.mode == opc::TargetMode::External) continue;
            if (rel.type == opc::rt::header || rel.type == opc::rt::footer || rel.type == opc::rt::footnotes ||
                rel.type == opc::rt::endnotes || rel.type == opc::rt::comments)
                v.push_back(opc::Package::resolve(main_, rel.target));
        }
    }
    return v;
}

// ── ids ──────────────────────────────────────────────────────────────────

void Document::scan_ids() {
    if (ids_scanned_) return;
    ids_scanned_ = true;
    static const std::set<std::string> ann = {
        "w:ins", "w:del", "w:moveFrom", "w:moveTo", "w:rPrChange", "w:pPrChange", "w:sectPrChange",
        "w:tblPrChange", "w:trPrChange", "w:tcPrChange", "w:tblGridChange", "w:tblPrExChange",
        "w:numberingChange", "w:cellIns", "w:cellDel", "w:cellMerge", "w:comment", "w:commentRangeStart",
        "w:commentRangeEnd", "w:commentReference", "w:moveFromRangeStart", "w:moveFromRangeEnd",
        "w:moveToRangeStart", "w:moveToRangeEnd", "w:customXmlInsRangeStart", "w:customXmlDelRangeStart",
        "w:customXmlMoveFromRangeStart", "w:customXmlMoveToRangeStart", "w14:conflictIns", "w14:conflictDel"};
    for (auto& name : pkg_.part_names()) {
        std::string ct = pkg_.content_type_of(name);
        if (ct.find("wordprocessingml") == std::string::npos && ct.find("ms-word") == std::string::npos) continue;
        if (schema::part_root_type(ct) < 0) continue;
        auto& d = pkg_.xml(name);
        for (auto n : d.select_nodes("//*[@w:id]")) {
            auto e = n.node();
            std::string q = e.name();
            int v = e.attribute("w:id").as_int(-1);
            if (q == "w:bookmarkStart" || q == "w:bookmarkEnd") max_bm_id_ = std::max(max_bm_id_, v);
            else if (ann.count(q)) max_ann_id_ = std::max(max_ann_id_, v);
        }
    }
}

int Document::next_annotation_id() {
    scan_ids();
    // Word draws revision, comment and bookmark ids from one pool in practice;
    // staying above both keeps every id unique per element type.
    int n = std::max(max_ann_id_, max_bm_id_) + 1;
    max_ann_id_ = n;
    return n;
}

int Document::next_bookmark_id() {
    scan_ids();
    int n = std::max(max_ann_id_, max_bm_id_) + 1;
    max_bm_id_ = n;
    return n;
}

void Document::scan_hex_ids() {
    if (hex_scanned_) return;
    hex_scanned_ = true;
    for (auto& name : pkg_.part_names()) {
        std::string ct = pkg_.content_type_of(name);
        if (schema::part_root_type(ct) < 0) continue;
        auto& d = pkg_.xml(name);
        for (auto q : {"//@w14:paraId", "//@w14:textId", "//@w15:paraId", "//@w16cid:durableId", "//@w16cex:durableId"})
            for (auto n : d.select_nodes(q)) hex_ids_.insert(util::lower(n.attribute().value()));
    }
}

std::string Document::new_hex_id() {
    scan_hex_ids();
    for (;;) {
        std::uint32_t v = util::random_u32() & 0x7FFFFFFF;
        if (v == 0 || v == 0x7FFFFFFF) continue;
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", v);
        std::string s = buf;
        if (hex_ids_.insert(util::lower(s)).second) return s;
    }
}

void Document::ensure_para_ids(pugi::xml_node root) {
    for (auto n : root.select_nodes(".//w:p")) {
        auto p = n.node();
        if (!p.attribute("w14:paraId")) {
            auto a = p.prepend_attribute("w14:paraId");
            a = new_hex_id().c_str();
        }
        if (!p.attribute("w14:textId")) {
            auto a = p.insert_attribute_after("w14:textId", p.attribute("w14:paraId"));
            a = "77777777";  // Word writes this placeholder until text is next saved
        }
    }
}

// ── authoring ────────────────────────────────────────────────────────────

std::string Document::date() { return author.date.empty() ? util::now_iso8601() : author.date; }

bool Document::track_changes() {
    auto st = settings(false);
    if (!st) return false;
    auto t = st.child("w:trackRevisions");
    if (!t) return false;
    std::string v = xml::attr(t, "w:val", "true");
    return v != "false" && v != "0" && v != "off";
}

void Document::set_track_changes(bool on) {
    auto st = settings();
    if (on) {
        auto t = schema::ensure_child(st, "w:trackRevisions");
        xml::remove_attr(t, "w:val");
    } else {
        st.remove_child("w:trackRevisions");
    }
}

void Document::ensure_person(const std::string& name) {
    auto pp = people();
    for (auto p : pp.children("w15:person"))
        if (xml::attr(p, "w15:author") == name) return;
    auto p = pp.append_child("w15:person");
    xml::set_attr(p, "w15:author", name);
    auto pi = p.append_child("w15:presenceInfo");
    xml::set_attr(pi, "w15:providerId", "None");
    xml::set_attr(pi, "w15:userId", name);
}

void Document::touch(const std::string& part) {
    if (!pkg_.has(part)) return;
    auto root = pkg_.xml(part).document_element();
    xml::fix_declarations(root);
}

void Document::finalize() {
    for (auto& name : pkg_.part_names()) {
        auto* p = pkg_.part(name);
        if (!p || !p->xml || !p->dirty) continue;
        if (schema::part_root_type(p->content_type) < 0) continue;
        xml::fix_declarations(p->xml->document_element());
    }
    // core.xml: Word bumps lastModifiedBy / modified on every save.
    if (auto cp = core_props(false)) {
        if (auto m = cp.child("cp:lastModifiedBy")) m.text().set(author.name.c_str());
        if (auto m = cp.child("dcterms:modified")) m.text().set(util::now_iso8601().c_str());
    }
}

}  // namespace docx::wml
