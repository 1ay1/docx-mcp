// Content control data binding (Developer > XML Mapping) and repeating
// sections. Bound controls show a document property or an element of a
// custom XML part; Word keeps both in step, and so do we.
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/docops.hpp"
#include "wml/document.hpp"
#include "wml/edit.hpp"
#include "wml/structure.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

// Word's fixed store ids for the property parts.
constexpr const char* kCoreStore = "{6C3C8BC8-F283-45AE-878A-BAB7291924A1}";
constexpr const char* kAppStore = "{6668398D-A668-4E3E-A5EB-62B293D839F1}";
constexpr const char* kCoverStore = "{55AF091B-3C7A-41E3-B477-F2FDAA23CFDA}";
constexpr const char* kCoreNs = "xmlns:ns0='http://schemas.openxmlformats.org/package/2006/metadata/core-properties' "
                                "xmlns:ns1='http://purl.org/dc/elements/1.1/'";
constexpr const char* kCoreNsTerms = "xmlns:ns0='http://schemas.openxmlformats.org/package/2006/metadata/core-properties' "
                                     "xmlns:ns1='http://purl.org/dc/terms/'";
constexpr const char* kAppNs = "xmlns:ns0='http://schemas.openxmlformats.org/officeDocument/2006/extended-properties'";
constexpr const char* kDataNs = "urn:docx-mcp:data";

struct Prop { const char* store; const char* ns; const char* xpath; const char* key; };

// Property name -> where Word binds it (Insert > Quick Parts > Document Property).
const std::map<std::string, Prop>& props() {
    static const std::map<std::string, Prop> m = {
        {"title", {kCoreStore, kCoreNs, "/ns0:coreProperties[1]/ns1:title[1]", "title"}},
        {"subject", {kCoreStore, kCoreNs, "/ns0:coreProperties[1]/ns1:subject[1]", "subject"}},
        {"author", {kCoreStore, kCoreNs, "/ns0:coreProperties[1]/ns1:creator[1]", "creator"}},
        {"keywords", {kCoreStore, "xmlns:ns0='http://schemas.openxmlformats.org/package/2006/metadata/core-properties'", "/ns0:coreProperties[1]/ns0:keywords[1]", "keywords"}},
        {"comments", {kCoreStore, kCoreNs, "/ns0:coreProperties[1]/ns1:description[1]", "description"}},
        {"category", {kCoreStore, "xmlns:ns0='http://schemas.openxmlformats.org/package/2006/metadata/core-properties'", "/ns0:coreProperties[1]/ns0:category[1]", "category"}},
        {"status", {kCoreStore, "xmlns:ns0='http://schemas.openxmlformats.org/package/2006/metadata/core-properties'", "/ns0:coreProperties[1]/ns0:contentStatus[1]", "content_status"}},
        {"company", {kAppStore, kAppNs, "/ns0:Properties[1]/ns0:Company[1]", "Company"}},
        {"manager", {kAppStore, kAppNs, "/ns0:Properties[1]/ns0:Manager[1]", "Manager"}},
    };
    (void)kCoreNsTerms;
    (void)kCoverStore;
    return m;
}

std::string custom_store(Document& d, bool create, std::string* part_out = nullptr) {
    // our data store: a customXml item whose root is in kDataNs
    if (const auto* rels = d.pkg().rels_if(d.main())) {
        for (auto& r : rels->items) {
            if (r.type != opc::rt::customXml) continue;
            std::string part = opc::Package::resolve(d.main(), r.target);
            if (!d.pkg().has(part)) continue;
            auto root = d.pkg().xml(part).document_element();
            if (std::string(root.name()) != "data" || xml::attr(root, "xmlns") != kDataNs) continue;
            std::string props_part;
            if (const auto* pr = d.pkg().rels_if(part))
                for (auto& x : pr->items) if (x.type == opc::rt::customXmlProps) props_part = opc::Package::resolve(part, x.target);
            if (props_part.empty() || !d.pkg().has(props_part)) continue;
            if (part_out) *part_out = part;
            return xml::attr(d.pkg().xml(props_part).document_element(), "ds:itemID");
        }
    }
    if (!create) return "";
    std::string item = d.pkg().unique_name("/customXml/item", ".xml");
    std::string props_part = item;
    props_part.replace(props_part.rfind("item"), 4, "itemProps");
    std::string id = util::new_guid();
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<data xmlns=\"" + std::string(kDataNs) + "\"/>";
    d.pkg().add_part(item, "application/xml", util::Bytes(x.begin(), x.end()));
    std::string px = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n<ds:datastoreItem ds:itemID=\"" + id +
                     "\" xmlns:ds=\"http://schemas.openxmlformats.org/officeDocument/2006/customXml\"><ds:schemaRefs><ds:schemaRef ds:uri=\"" +
                     kDataNs + "\"/></ds:schemaRefs></ds:datastoreItem>";
    d.pkg().add_xml_part(props_part, opc::ct::customXmlProps, px);
    d.pkg().rels(item).add(opc::rt::customXmlProps, opc::Package::relative(item, props_part));
    d.pkg().rels(d.main()).add(opc::rt::customXml, opc::Package::relative(d.main(), item));
    if (part_out) *part_out = item;
    return id;
}

// "a/b/c" -> the element in the data store (created on demand)
pugi::xml_node data_node(Document& d, const std::string& path, bool create) {
    std::string part;
    if (custom_store(d, create, &part).empty()) return {};
    auto node = d.pkg().xml(part).document_element();
    for (auto& seg0 : util::split(path, '/')) {
        std::string seg = util::trim(seg0);
        if (seg.empty()) continue;
        for (char c : seg)
            if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
                throw std::runtime_error("data paths are names separated by / (letters, digits, _ - .), like client/name");
        if (std::isdigit((unsigned char)seg[0]) || seg[0] == '-' || seg[0] == '.')
            throw std::runtime_error("data path names start with a letter or _");
        auto c = node.child(seg.c_str());
        if (!c) {
            if (!create) return {};
            c = node.append_child(seg.c_str());
        }
        node = c;
    }
    return node;
}

std::string data_xpath(const std::string& path) {
    std::string o;
    o += "/ns0:data[1]";
    for (auto& seg0 : util::split(path, '/')) {
        std::string seg = util::trim(seg0);
        if (!seg.empty()) o += "/ns0:" + seg + "[1]";
    }
    return o;
}

// The value a binding points at, or nullopt.
std::optional<std::string> bound_value(Document& d, pugi::xml_node db) {
    std::string store = xml::attr(db, "w:storeItemID"), xp = xml::attr(db, "w:xpath");
    for (auto& [name, p] : props())
        if (util::iequals(store, p.store) && xp == p.xpath) {
            Json j = std::string(p.store) == kCoreStore ? core_properties(d) : app_properties(d);
            if (j.contains(p.key) && j[p.key].is_string()) return j[p.key].get<std::string>();
            return std::string();
        }
    std::string part;
    std::string ours = custom_store(d, false, &part);
    if (!ours.empty() && util::iequals(store, ours) && xp.rfind("/ns0:data[1]", 0) == 0) {
        std::string path;
        std::size_t i = std::string("/ns0:data[1]").size();
        while (i < xp.size()) {
            auto a = xp.find("/ns0:", i);
            if (a == std::string::npos) break;
            auto b = xp.find('[', a);
            path += (path.empty() ? "" : "/") + xp.substr(a + 5, b - a - 5);
            i = xp.find(']', b) + 1;
        }
        auto n = data_node(d, path, false);
        if (!n) return std::string();
        return std::string(n.text().get());
    }
    return std::nullopt;
}

pugi::xml_node find_sdt(Document& d, const std::string& key, std::string* part_out = nullptr) {
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        for (auto sdt : xml::descendants(d.pkg().xml(part).document_element(), "w:sdt")) {
            auto pr = sdt.child("w:sdtPr");
            if (xml::attr(pr.child("w:id"), "w:val") == key || xml::attr(pr.child("w:tag"), "w:val") == key ||
                xml::attr(pr.child("w:alias"), "w:val") == key) {
                if (part_out) *part_out = part;
                return sdt;
            }
        }
    }
    return {};
}

void show_text(Document& d, pugi::xml_node sdt, const std::string& value) {
    auto pr = sdt.child("w:sdtPr");
    std::string key = xml::attr(pr.child("w:id"), "w:val");
    if (value.empty()) return;  // empty stays as the placeholder
    // reuse set_content_control's text replacement, without touching the binding
    auto db = pr.child("w:dataBinding");
    pugi::xml_document keep;
    if (db) { keep.append_copy(db); pr.remove_child(db); }
    auto restore = [&]() {
        if (keep.first_child()) { auto c = pr.append_copy(keep.first_child()); schema::place_child(pr, c); }
    };
    try {
        set_content_control(d, key, value);
    } catch (...) {
        restore();
        throw;
    }
    restore();
}

}  // namespace

void bind_content_control(Document& d, const std::string& key, const std::string& property, const std::string& path) {
    auto sdt = find_sdt(d, key);
    if (!sdt) throw std::runtime_error("content control '" + key + "' not found (use id, tag or title)");
    auto pr = sdt.child("w:sdtPr");
    if (pr.child("w14:checkbox") || pr.child("w:picture") || pr.child("w:group") || pr.child("w15:repeatingSection"))
        throw std::runtime_error("only text, rich text, date and drop-down controls can be bound");
    // check the target first so a bad one leaves the control as it was
    const Prop* prop = nullptr;
    if (!property.empty()) {
        auto it = props().find(util::lower(property));
        if (it == props().end()) {
            std::vector<std::string_view> valid;
            for (auto& [k, v] : props()) valid.push_back(k);
            throw std::runtime_error(util::unknown_key("document property", property, valid));
        }
        prop = &it->second;
    } else if (!path.empty()) {
        data_xpath(path);
        for (auto& seg0 : util::split(path, '/')) {
            std::string seg = util::trim(seg0);
            if (seg.empty()) continue;
            for (char c : seg)
                if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
                    throw std::runtime_error("data paths are names separated by / (letters, digits, _ - .), like client/name");
            if (std::isdigit((unsigned char)seg[0]) || seg[0] == '-' || seg[0] == '.')
                throw std::runtime_error("data path names start with a letter or _");
        }
    }
    if (auto old = pr.child("w:dataBinding")) pr.remove_child(old);
    if (property.empty() && path.empty()) return;  // unbind
    auto db = schema::ensure_child(pr, "w:dataBinding");
    std::string value;
    if (prop) {
        auto it = props().find(util::lower(property));
        xml::set_attr(db, "w:prefixMappings", it->second.ns);
        xml::set_attr(db, "w:xpath", it->second.xpath);
        xml::set_attr(db, "w:storeItemID", it->second.store);
        // Word stores the property in the document's own parts; if the control has text and the property doesn't, the control wins
        auto cur = bound_value(d, db);
        std::string shown;
        for (auto c : list_content_controls(d)) if (c.id == xml::attr(pr.child("w:id"), "w:val")) shown = c.text;
        if (cur && cur->empty() && !shown.empty()) {
            if (std::string(it->second.store) == kCoreStore) set_core_properties(d, {{it->second.key, shown}});
            else set_app_property(d, it->second.key, shown);
        }
        value = bound_value(d, db).value_or("");
    } else {
        std::string id = custom_store(d, true);
        auto n = data_node(d, path, true);
        xml::set_attr(db, "w:prefixMappings", std::string("xmlns:ns0='") + kDataNs + "'");
        xml::set_attr(db, "w:xpath", data_xpath(path));
        xml::set_attr(db, "w:storeItemID", id);
        std::string shown;
        for (auto c : list_content_controls(d)) if (c.id == xml::attr(pr.child("w:id"), "w:val")) shown = c.text;
        if (std::string(n.text().get()).empty() && !shown.empty()) n.text().set(shown.c_str());
        value = n.text().get();
    }
    schema::place_child(pr, db);
    show_text(d, sdt, value);
}

int refresh_bound_controls(Document& d) {
    int n = 0;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        std::vector<pugi::xml_node> sdts;
        for (auto sdt : xml::descendants(d.pkg().xml(part).document_element(), "w:sdt"))
            if (sdt.child("w:sdtPr").child("w:dataBinding")) sdts.push_back(sdt);
        for (auto sdt : sdts) {
            auto v = bound_value(d, sdt.child("w:sdtPr").child("w:dataBinding"));
            if (!v || v->empty()) continue;
            std::string shown;
            for (auto t : xml::descendants(sdt.child("w:sdtContent"), "w:t")) shown += t.text().get();
            if (shown == *v && !sdt.child("w:sdtPr").child("w:showingPlcHdr")) continue;
            show_text(d, sdt, *v);
            n++;
        }
    }
    return n;
}

void set_bound_value(Document& d, pugi::xml_node sdt, const std::string& value) {
    auto db = sdt.child("w:sdtPr").child("w:dataBinding");
    if (!db) return;
    std::string store = xml::attr(db, "w:storeItemID"), xp = xml::attr(db, "w:xpath");
    for (auto& [name, p] : props())
        if (util::iequals(store, p.store) && xp == p.xpath) {
            if (std::string(p.store) == kCoreStore) set_core_properties(d, {{p.key, value}});
            else set_app_property(d, p.key, value);
            return;
        }
    std::string ours = custom_store(d, false);
    if (!ours.empty() && util::iequals(store, ours)) {
        std::string path;
        std::size_t i = std::string("/ns0:data[1]").size();
        while (i < xp.size()) {
            auto a = xp.find("/ns0:", i);
            if (a == std::string::npos) break;
            auto b = xp.find('[', a);
            path += (path.empty() ? "" : "/") + xp.substr(a + 5, b - a - 5);
            i = xp.find(']', b) + 1;
        }
        if (auto node = data_node(d, path, true)) node.text().set(util::xml_safe(value).c_str());
    }
}

// ── repeating sections ──

pugi::xml_node add_repeating_section(Document& d, pugi::xml_node first, pugi::xml_node last, const std::string& tag) {
    if (first.parent() != last.parent()) throw std::runtime_error("the section's first and last paragraphs must be siblings");
    if (!xml::name_is(first.parent(), "w:body") && !xml::name_is(first.parent(), "w:sdtContent") && !xml::name_is(first.parent(), "w:tc"))
        throw std::runtime_error("repeating sections wrap whole paragraphs or tables");
    auto parent = first.parent();
    auto outer = parent.insert_child_before("w:sdt", first);
    auto pr = outer.append_child("w:sdtPr");
    if (!tag.empty()) {
        xml::set_attr(pr.append_child("w:alias"), "w:val", tag);
        xml::set_attr(pr.append_child("w:tag"), "w:val", tag);
    }
    xml::set_attr(pr.append_child("w:id"), "w:val", std::to_string((std::int32_t)util::random_u32()));
    pr.append_child("w15:repeatingSection");
    schema::sort_children(pr);
    auto content = outer.append_child("w:sdtContent");
    // one item holding the blocks
    auto item = content.append_child("w:sdt");
    auto ipr = item.append_child("w:sdtPr");
    xml::set_attr(ipr.append_child("w:id"), "w:val", std::to_string((std::int32_t)util::random_u32()));
    ipr.append_child("w15:repeatingSectionItem");
    auto icontent = item.append_child("w:sdtContent");
    std::vector<pugi::xml_node> mv;
    for (auto n = first; n; n = n.next_sibling()) {
        mv.push_back(n);
        if (n == last) break;
    }
    for (auto n : mv) {
        if (xml::name_is(n, "w:sectPr") || n.child("w:pPr").child("w:sectPr"))
            throw std::runtime_error("a repeating section can't include a section break");
        icontent.append_move(n);
    }
    xml::declare(xml::root_of(parent), "w15", true);
    return outer;
}

int add_repeating_item(Document& d, const std::string& key, int count) {
    auto sdt = find_sdt(d, key);
    if (!sdt || !sdt.child("w:sdtPr").child("w15:repeatingSection"))
        throw std::runtime_error("no repeating section '" + key + "' (list_content_controls shows them)");
    auto content = sdt.child("w:sdtContent");
    pugi::xml_node last_item;
    for (auto c : content.children("w:sdt"))
        if (c.child("w:sdtPr").child("w15:repeatingSectionItem")) last_item = c;
    if (!last_item) throw std::runtime_error("the repeating section has no item to copy");
    count = std::clamp(count, 1, 500);
    for (int i = 0; i < count; i++) {
        auto copy = content.insert_copy_after(last_item, last_item);
        // fresh ids for the item and every control and paragraph inside
        for (auto id : copy.select_nodes(".//w:sdtPr/w:id")) xml::set_attr(id.node(), "w:val", std::to_string((std::int32_t)util::random_u32()));
        if (auto id = copy.child("w:sdtPr").child("w:id")) xml::set_attr(id, "w:val", std::to_string((std::int32_t)util::random_u32()));
        for (auto p : copy.select_nodes(".//w:p")) {
            p.node().remove_attribute("w14:paraId");
            p.node().remove_attribute("w14:textId");
        }
        d.ensure_para_ids(copy);
        last_item = copy;
    }
    return count;
}

}  // namespace docx::wml
