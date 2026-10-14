#include "wml/styles.hpp"

#include <algorithm>
#include <map>
#include <cctype>
#include <functional>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/assets.hpp"
#include "util/util.hpp"
#include "wml/edit.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

using Json = nlohmann::json;

namespace {

const pugi::xml_document& builtins() {
    static pugi::xml_document* d = [] {
        auto* doc = new pugi::xml_document;
        auto s = assets::get("word/builtin_styles.xml");
        doc->load_buffer(s.data(), s.size());
        // curated Word 2013+ definitions replace the harvested ones
        pugi::xml_document ov;
        auto o = assets::get("word/builtin_overrides.xml");
        ov.load_buffer(o.data(), o.size());
        auto root = doc->document_element();
        for (auto st : ov.document_element().children("w:style")) {
            std::string id = st.attribute("w:styleId").value();
            for (auto old : root.children("w:style"))
                if (id == old.attribute("w:styleId").value()) { root.remove_child(old); break; }
            root.append_copy(st);
        }
        return doc;
    }();
    return *d;
}

std::string norm_name(std::string s) {
    std::string o;
    for (char c : s) if (c != ' ') o += char(std::tolower((unsigned char)c));
    return o;
}

pugi::xml_node builtin(const std::string& key) {
    auto root = builtins().document_element();
    std::string k = norm_name(key);
    for (auto s : root.children("w:style")) {
        if (norm_name(xml::attr(s, "w:styleId")) == k) return s;
        if (norm_name(xml::attr(s.child("w:name"), "w:val")) == k) return s;
    }
    // UI names that differ from the internal name
    static const std::map<std::string, std::string> ui = {
        {"commenttext", "CommentText"}, {"commentreference", "CommentReference"}, {"commentsubject", "CommentSubject"},
        {"normaltable", "TableNormal"}, {"tablenormal", "TableNormal"}};
    if (auto it = ui.find(k); it != ui.end()) return builtin(it->second);
    return {};
}

}  // namespace

pugi::xml_node find_style(Document& d, const std::string& key) {
    auto st = d.styles(false);
    if (!st) return {};
    for (auto s : st.children("w:style"))
        if (xml::attr(s, "w:styleId") == key) return s;
    std::string k = norm_name(key);
    for (auto s : st.children("w:style"))
        if (norm_name(xml::attr(s.child("w:name"), "w:val")) == k || norm_name(xml::attr(s, "w:styleId")) == k) return s;
    return {};
}

std::string ensure_style(Document& d, const std::string& key) {
    if (auto s = find_style(d, key)) return xml::attr(s, "w:styleId");
    auto b = builtin(key);
    if (!b) throw std::runtime_error("style '" + key + "' not found (and not a Word built-in)");
    auto st = d.styles();
    // add it first so link cycles (Heading1 <-> Heading1Char) terminate
    auto n = st.append_copy(b);
    for (auto q : {"w:basedOn", "w:link", "w:next"}) {
        std::string dep = xml::attr(n.child(q), "w:val");
        if (!dep.empty() && !find_style(d, dep) && builtin(dep)) ensure_style(d, dep);
    }
    // drop links/next to styles we couldn't provide
    for (auto q : {"w:basedOn", "w:link", "w:next"}) {
        auto e = n.child(q);
        if (e && !find_style(d, xml::attr(e, "w:val"))) n.remove_child(e);
    }
    xml::fix_declarations(xml::root_of(st));
    return xml::attr(n, "w:styleId");
}

std::vector<StyleInfo> list_styles(Document& d, bool latent) {
    std::vector<StyleInfo> v;
    std::set<std::string> used;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto q : {"//w:pStyle", "//w:rStyle", "//w:tblStyle"})
            for (auto n : root.select_nodes(q)) used.insert(n.node().attribute("w:val").value());
    }
    std::set<std::string> have;
    if (auto st = d.styles(false)) {
        for (auto s : st.children("w:style")) {
            StyleInfo i;
            i.id = xml::attr(s, "w:styleId");
            i.name = xml::attr(s.child("w:name"), "w:val");
            i.type = xml::attr(s, "w:type", "paragraph");
            i.based_on = xml::attr(s.child("w:basedOn"), "w:val");
            i.next = xml::attr(s.child("w:next"), "w:val");
            i.link = xml::attr(s.child("w:link"), "w:val");
            i.custom = xml::attr(s, "w:customStyle") == "1" || xml::attr(s, "w:customStyle") == "true";
            i.builtin = !i.custom;
            i.in_use = used.count(i.id) > 0 || xml::attr(s, "w:default") == "1";
            i.hidden = s.child("w:semiHidden") || s.child("w:hidden");
            i.quick = s.child("w:qFormat");
            if (auto u = s.child("w:uiPriority")) i.ui_priority = std::stoi(xml::attr(u, "w:val", "-1"));
            have.insert(i.id);
            v.push_back(i);
        }
    }
    if (latent) {
        for (auto s : builtins().document_element().children("w:style")) {
            std::string id = xml::attr(s, "w:styleId");
            if (have.count(id)) continue;
            StyleInfo i;
            i.id = id;
            i.name = xml::attr(s.child("w:name"), "w:val");
            i.type = xml::attr(s, "w:type", "paragraph");
            i.builtin = true;
            i.based_on = xml::attr(s.child("w:basedOn"), "w:val");
            i.hidden = true;  // not yet in the document; added on first use
            v.push_back(i);
        }
    }
    return v;
}

std::string upsert_style(Document& d, const std::string& id_in, const Json& props) {
    auto st = d.styles();
    std::string id = id_in;
    pugi::xml_node s = find_style(d, id);
    if (!s && builtin(id)) { ensure_style(d, id); s = find_style(d, id); }
    if (!s) {
        // Word derives the id from the name by dropping spaces
        if (id.empty() && props.contains("name")) {
            for (char c : props["name"].get<std::string>()) if (c != ' ') id += c;
        }
        if (id.empty()) throw std::runtime_error("style id or name required");
        s = st.append_child("w:style");
        xml::set_attr(s, "w:type", props.value("type", std::string("paragraph")));
        xml::set_attr(s, "w:customStyle", "1");
        xml::set_attr(s, "w:styleId", id);
        xml::set_attr(schema::ensure_child(s, "w:name"), "w:val", props.value("name", id));
        std::string type = props.value("type", std::string("paragraph"));
        if (!props.contains("based_on")) {
            if (type == "paragraph") xml::set_attr(schema::ensure_child(s, "w:basedOn"), "w:val", ensure_style(d, "Normal"));
            else if (type == "character") xml::set_attr(schema::ensure_child(s, "w:basedOn"), "w:val", ensure_style(d, "DefaultParagraphFont"));
            else if (type == "table") xml::set_attr(schema::ensure_child(s, "w:basedOn"), "w:val", ensure_style(d, "TableNormal"));
        }
        if (type == "paragraph" && !props.contains("next")) xml::set_attr(schema::ensure_child(s, "w:next"), "w:val", id);
        if (!props.contains("quick_style")) schema::ensure_child(s, "w:qFormat");
    }
    for (auto& [k, v] : props.items()) {
        if (k == "name") xml::set_attr(schema::ensure_child(s, "w:name"), "w:val", v.get<std::string>());
        else if (k == "type") {
            if (xml::attr(s, "w:type") != v.get<std::string>()) {
                enum_attr(s, "w:type", v.get<std::string>());
            }
        } else if (k == "based_on" || k == "next" || k == "link") {
            const char* q = k == "based_on" ? "w:basedOn" : k == "next" ? "w:next" : "w:link";
            if (v.is_null()) { s.remove_child(q); continue; }
            xml::set_attr(schema::ensure_child(s, q), "w:val", ensure_style(d, v.get<std::string>()));
        } else if (k == "ui_priority") xml::set_attr(schema::ensure_child(s, "w:uiPriority"), "w:val", std::to_string(v.get<int>()));
        else if (k == "quick_style") { if (v.get<bool>()) schema::ensure_child(s, "w:qFormat"); else s.remove_child("w:qFormat"); }
        else if (k == "hidden") { if (v.get<bool>()) schema::ensure_child(s, "w:semiHidden"); else s.remove_child("w:semiHidden"); }
        else if (k == "paragraph") {
            auto ppr = schema::ensure_child(s, "w:pPr");
            apply_ppr_json(d, ppr, v);
            if (!ppr.first_child()) s.remove_child(ppr);
        } else if (k == "run") {
            auto rpr = schema::ensure_child(s, "w:rPr");
            apply_rpr_json(d, rpr, v);
            if (!rpr.first_child()) s.remove_child(rpr);
        } else if (k == "default") {
            if (v.get<bool>()) {
                std::string type = xml::attr(s, "w:type");
                for (auto o : st.children("w:style"))
                    if (xml::attr(o, "w:type") == type) xml::remove_attr(o, "w:default");
                xml::set_attr(s, "w:default", "1");
            }
        } else {
            throw std::runtime_error("unknown style property '" + k + "'");
        }
    }
    return xml::attr(s, "w:styleId");
}

void delete_style(Document& d, const std::string& id) {
    auto s = find_style(d, id);
    if (!s) throw std::runtime_error("style '" + id + "' not found");
    if (xml::attr(s, "w:default") == "1") throw std::runtime_error("can't delete a default style");
    std::string sid = xml::attr(s, "w:styleId");
    std::string type = xml::attr(s, "w:type", "paragraph");
    std::string base = xml::attr(s.child("w:basedOn"), "w:val");
    // Word: paragraphs using it revert to Normal; styles based on it rebase
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        const char* q = type == "character" ? "//w:rStyle" : type == "table" ? "//w:tblStyle" : "//w:pStyle";
        for (auto n : root.select_nodes(q)) {
            auto e = n.node();
            if (xml::attr(e, "w:val") == sid) e.parent().remove_child(e);
        }
    }
    auto st = d.styles();
    for (auto o : st.children("w:style")) {
        auto b = o.child("w:basedOn");
        if (b && xml::attr(b, "w:val") == sid) {
            if (base.empty()) o.remove_child(b);
            else xml::set_attr(b, "w:val", base);
        }
        for (auto q : {"w:next", "w:link"}) {
            auto e = o.child(q);
            if (e && xml::attr(e, "w:val") == sid) o.remove_child(e);
        }
    }
    st.remove_child(s);
}

// ── effective formatting ─────────────────────────────────────────────────

namespace {

// Merge child elements of `src` into `dst` (later wins), toggles XOR per
// ECMA 17.7.3 for the style hierarchy.
void merge_props(pugi::xml_node dst, pugi::xml_node src, bool toggles_xor) {
    static const std::set<std::string> toggles = {"w:b", "w:bCs", "w:i", "w:iCs", "w:caps", "w:smallCaps",
                                                  "w:strike", "w:dstrike", "w:outline", "w:shadow", "w:emboss",
                                                  "w:imprint", "w:vanish"};
    for (auto c : src.children()) {
        if (c.type() != pugi::node_element) continue;
        std::string q = c.name();
        if (q == "w:rPrChange" || q == "w:pPrChange" || q == "w:ins" || q == "w:del" || q == "w:rPr" || q == "w:sectPr") continue;
        auto ex = dst.child(q.c_str());
        if (toggles_xor && toggles.count(q) && ex) {
            auto on = [](pugi::xml_node e) { std::string v = xml::attr(e, "w:val", "1"); return !(v == "0" || v == "false" || v == "off"); };
            bool r = on(ex) != on(c);
            dst.remove_child(ex);
            auto n = dst.append_child(q.c_str());
            if (!r) xml::set_attr(n, "w:val", "0");
            continue;
        }
        if (ex && (q == "w:spacing" || q == "w:ind" || q == "w:rFonts" || q == "w:lang" || q == "w:pBdr")) {
            // attribute-level merge
            for (auto a : c.attributes()) xml::set_attr(ex, a.name(), a.value());
            continue;
        }
        if (ex) dst.remove_child(ex);
        dst.append_copy(c);
    }
}

std::vector<pugi::xml_node> style_chain(Document& d, const std::string& id) {
    std::vector<pugi::xml_node> chain;
    std::set<std::string> seen;
    for (std::string cur = id; !cur.empty() && !seen.count(cur);) {
        seen.insert(cur);
        auto s = find_style(d, cur);
        if (!s) break;
        chain.push_back(s);
        cur = xml::attr(s.child("w:basedOn"), "w:val");
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

pugi::xml_node default_style(Document& d, const char* type) {
    auto st = d.styles(false);
    if (!st) return {};
    for (auto s : st.children("w:style"))
        if (xml::attr(s, "w:type") == type && (xml::attr(s, "w:default") == "1" || xml::attr(s, "w:default") == "true")) return s;
    return {};
}

}  // namespace

Json effective_paragraph(Document& d, pugi::xml_node p) {
    pugi::xml_document tmp;
    auto out = tmp.append_child("w:pPr");
    auto st = d.styles(false);
    if (st) merge_props(out, st.child("w:docDefaults").child("w:pPrDefault").child("w:pPr"), false);
    std::string sid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
    if (sid.empty()) if (auto ds = default_style(d, "paragraph")) sid = xml::attr(ds, "w:styleId");
    for (auto s : style_chain(d, sid)) merge_props(out, s.child("w:pPr"), false);
    merge_props(out, p.child("w:pPr"), false);
    Json j = ppr_to_json(out);
    j["style"] = sid;
    return j;
}

Json effective_run(Document& d, pugi::xml_node p, pugi::xml_node run) {
    pugi::xml_document tmp;
    auto out = tmp.append_child("w:rPr");
    auto st = d.styles(false);
    if (st) merge_props(out, st.child("w:docDefaults").child("w:rPrDefault").child("w:rPr"), false);
    std::string psid = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
    if (psid.empty()) if (auto ds = default_style(d, "paragraph")) psid = xml::attr(ds, "w:styleId");
    pugi::xml_document acc;
    auto style_rpr = acc.append_child("w:rPr");
    for (auto s : style_chain(d, psid)) merge_props(style_rpr, s.child("w:rPr"), true);
    std::string rsid = xml::attr(run.child("w:rPr").child("w:rStyle"), "w:val");
    for (auto s : style_chain(d, rsid)) merge_props(style_rpr, s.child("w:rPr"), true);
    merge_props(out, style_rpr, false);
    merge_props(out, run.child("w:rPr"), false);
    // theme fonts -> names
    Json j = rpr_to_json(out);
    if (auto f = out.child("w:rFonts")) {
        std::string th = xml::attr(f, "w:asciiTheme");
        if (!th.empty()) {
            std::string theme_part = d.related(opc::rt::theme);
            if (!theme_part.empty()) {
                auto t = d.pkg().xml(theme_part).document_element();
                auto fs = xml::first_descendant(t, "a:fontScheme");
                auto which = th.find("major") == 0 ? fs.child("a:majorFont") : fs.child("a:minorFont");
                j["font"] = xml::attr(which.child("a:latin"), "typeface");
            }
        }
    }
    if (!j.contains("size")) j["size"] = 10.0;  // Word's built-in default when nothing sets it
    return j;
}

}  // namespace docx::wml
