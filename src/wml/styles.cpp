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
#include "wml/structure.hpp"
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

pugi::xml_node builtin_style(const std::string& key) { return builtin(key); }

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
    if (!b) {
        // the names a caller could have meant, best match first
        std::vector<std::string> names;
        auto add = [&](pugi::xml_node s) {
            std::string n = xml::attr(s.child("w:name"), "w:val", xml::attr(s, "w:styleId"));
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        };
        if (auto st = d.styles()) for (auto s : st.children("w:style")) add(s);
        for (auto s : builtins().document_element().children("w:style")) add(s);
        // rank by words in common with what was asked ("Light Grid Accent 1"
        // is close to "Grid Table 1 Light Accent 1"), then by length
        auto words = [](const std::string& s) {
            std::vector<std::string> w;
            std::string cur;
            for (char c : s + " ") {
                if (std::isalnum((unsigned char)c)) cur += char(std::tolower((unsigned char)c));
                else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
            }
            return w;
        };
        auto want = words(key);
        auto score = [&](const std::string& n) {
            int sc = 0;
            for (auto& w : words(n)) sc += std::find(want.begin(), want.end(), w) != want.end();
            return sc;
        };
        std::stable_sort(names.begin(), names.end(), [&](auto& a, auto& b) {
            int sa = score(a), sb = score(b);
            return sa != sb ? sa > sb : a.size() < b.size();
        });
        std::string m = "style '" + key + "' not found. Closest: ";
        for (std::size_t i = 0; i < names.size() && i < 6; i++) m += (i ? ", " : "") + names[i];
        m += ". list_styles {include_builtin: true} shows them all.";
        throw std::runtime_error(m);
    }
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

Json import_styles(Document& d, Document& src, const std::vector<std::string>& names, bool overwrite, bool defaults) {
    auto from = src.styles(false);
    if (!from) throw std::runtime_error("the source document has no styles");
    auto to = d.styles();
    Json added = Json::array(), replaced = Json::array(), skipped = Json::array();
    // which source styles: the named ones (by id or name) plus their basedOn / link / next chains
    std::vector<pugi::xml_node> want;
    std::set<std::string> seen;
    std::function<void(pugi::xml_node)> take = [&](pugi::xml_node s) {
        std::string id = xml::attr(s, "w:styleId");
        if (!seen.insert(id).second) return;
        for (auto q : {"w:basedOn", "w:link", "w:next"}) {
            std::string dep = xml::attr(s.child(q), "w:val");
            if (dep.empty()) continue;
            for (auto o : from.children("w:style"))
                if (xml::attr(o, "w:styleId") == dep) take(o);
        }
        want.push_back(s);
    };
    if (names.empty()) {
        for (auto s : from.children("w:style")) take(s);
    } else {
        for (auto& n : names) {
            pugi::xml_node hit;
            for (auto s : from.children("w:style"))
                if (xml::attr(s, "w:styleId") == n || norm_name(xml::attr(s.child("w:name"), "w:val")) == norm_name(n)) hit = s;
            if (!hit) throw std::runtime_error("the source document has no style '" + n + "'");
            take(hit);
        }
    }
    // numbering a style refers to comes along with fresh ids
    std::map<std::string, std::string> num_map;
    auto bring_num = [&](const std::string& nid) -> std::string {
        if (nid.empty() || nid == "0") return nid;
        if (auto it = num_map.find(nid); it != num_map.end()) return it->second;
        auto sn = src.numbering(false);
        if (!sn) return "";
        pugi::xml_node num;
        for (auto n : sn.children("w:num")) if (xml::attr(n, "w:numId") == nid) num = n;
        if (!num) return "";
        std::string aid = xml::attr(num.child("w:abstractNumId"), "w:val");
        pugi::xml_node abs;
        for (auto a : sn.children("w:abstractNum")) if (xml::attr(a, "w:abstractNumId") == aid) abs = a;
        if (!abs) return "";
        auto dn = d.numbering();
        int na = 0, nn = 1;
        for (auto a : dn.children("w:abstractNum")) na = std::max(na, a.attribute("w:abstractNumId").as_int() + 1);
        for (auto n : dn.children("w:num")) nn = std::max(nn, n.attribute("w:numId").as_int() + 1);
        auto ca = schema::insert_child(dn, "w:abstractNum");
        for (auto at : abs.attributes()) ca.append_attribute(at.name()) = at.value();
        for (auto c : abs.children()) ca.append_copy(c);
        xml::set_attr(ca, "w:abstractNumId", std::to_string(na));
        // numStyleLink / styleLink point at styles we may not bring: drop them
        if (auto l = ca.child("w:numStyleLink")) ca.remove_child(l);
        auto cn = schema::insert_child(dn, "w:num");
        xml::set_attr(cn, "w:numId", std::to_string(nn));
        xml::set_attr(schema::ensure_child(cn, "w:abstractNumId"), "w:val", std::to_string(na));
        for (auto ov : num.children("w:lvlOverride")) cn.append_copy(ov);
        xml::fix_declarations(xml::root_of(dn));
        return num_map[nid] = std::to_string(nn);
    };
    for (auto s : want) {
        std::string id = xml::attr(s, "w:styleId");
        pugi::xml_node existing;
        for (auto o : to.children("w:style")) if (xml::attr(o, "w:styleId") == id) existing = o;
        if (existing && !overwrite) { skipped.push_back(id); continue; }
        pugi::xml_node n = existing ? to.insert_copy_after(s, existing) : to.append_copy(s);
        if (existing) { to.remove_child(existing); replaced.push_back(id); }
        else added.push_back(id);
        if (auto np = n.child("w:pPr").child("w:numPr")) {
            auto idn = np.child("w:numId");
            std::string mapped = bring_num(xml::attr(idn, "w:val"));
            if (mapped.empty()) n.child("w:pPr").remove_child(np);
            else xml::set_attr(idn, "w:val", mapped);
        }
        // revision marks and rsids belong to the source
        for (auto q : {"w:rsid", "w:pPrChange", "w:rPrChange"})
            for (auto e : xml::descendants(n, q)) e.parent().remove_child(e);
    }
    if (defaults) {
        if (auto dd = from.child("w:docDefaults")) {
            if (auto old = to.child("w:docDefaults")) to.remove_child(old);
            auto c = to.prepend_copy(dd);
            schema::place_child(to, c);
        }
    }
    // links to styles neither side has would be dangling
    for (auto s : to.children("w:style"))
        for (auto q : {"w:basedOn", "w:link", "w:next"}) {
            auto e = s.child(q);
            if (!e) continue;
            bool ok = false;
            for (auto o : to.children("w:style")) if (xml::attr(o, "w:styleId") == xml::attr(e, "w:val")) ok = true;
            if (!ok) s.remove_child(e);
        }
    // only one default per type
    std::set<std::string> default_types;
    for (auto s : to.children("w:style")) {
        if (xml::attr(s, "w:default") != "1") continue;
        std::string t = xml::attr(s, "w:type", "paragraph");
        if (!default_types.insert(t).second) xml::remove_attr(s, "w:default");
    }
    xml::fix_declarations(xml::root_of(to));
    Json out = {{"added", added}, {"replaced", replaced}};
    if (!skipped.empty()) out["kept_existing"] = skipped;
    return out;
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
            if (auto u = s.child("w:uiPriority")) i.ui_priority = util::to_int(xml::attr(u, "w:val", "-1"));
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
    // no id: the name picks the style (or Word's derived id for a new one)
    if (id.empty() && props.contains("name")) {
        if (auto byname = find_style(d, props["name"].get<std::string>())) id = xml::attr(byname, "w:styleId");
        else for (char c : props["name"].get<std::string>()) if (c != ' ') id += c;
    }
    pugi::xml_node s = id.empty() ? pugi::xml_node() : find_style(d, id);
    if (!s && !id.empty() && builtin(id)) { ensure_style(d, id); s = find_style(d, id); }
    if (!s) {
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
        } else if (k == "table" || k == "conditional") {
            if (xml::attr(s, "w:type") != "table") throw std::runtime_error(k + " applies to table styles (type table)");
            apply_table_style(d, s, k == "table" ? v : Json(), k == "conditional" ? v : Json());
        } else {
            throw std::runtime_error(util::unknown_key("style property", k, {"name", "type", "based_on", "next", "link", "ui_priority", "quick_style", "hidden", "paragraph", "run", "default", "table", "conditional"}));
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
