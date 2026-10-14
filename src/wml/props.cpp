// Run / paragraph property JSON <-> WordprocessingML. Names follow Word's
// object model (Font.Bold, ParagraphFormat.SpaceAfter ...) in snake_case.
// Measurements: font sizes in points, spacing/indents in points unless a
// unit suffix is given ("0.5in", "1cm", "12pt", "240tw").
#include <cmath>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/edit.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

// Parse a length into twips. Plain numbers are points.
long twips(const Json& v) {
    if (v.is_number()) return std::lround(v.get<double>() * 20);
    std::string s = util::trim(v.get<std::string>());
    auto num = [&](std::size_t n) { return std::stod(s.substr(0, s.size() - n)); };
    if (s.ends_with("tw") || s.ends_with("twips")) return std::lround(std::stod(s));
    if (s.ends_with("pt")) return std::lround(num(2) * 20);
    if (s.ends_with("in")) return std::lround(num(2) * 1440);
    if (s.ends_with("cm")) return std::lround(num(2) * 1440 / 2.54);
    if (s.ends_with("mm")) return std::lround(num(2) * 144 / 2.54);
    if (s.ends_with("pc") || s.ends_with("pi")) return std::lround(num(2) * 240);
    return std::lround(std::stod(s) * 20);
}

std::string hexcolor(const Json& v) {
    std::string s = v.get<std::string>();
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (util::iequals(s, "auto")) return "auto";
    static const std::map<std::string, std::string> names = {
        {"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"}, {"green", "00B050"}, {"blue", "0070C0"},
        {"yellow", "FFFF00"}, {"orange", "FFC000"}, {"purple", "7030A0"}, {"gray", "808080"}, {"grey", "808080"},
        {"darkred", "C00000"}, {"darkblue", "002060"}, {"lightblue", "00B0F0"}, {"lightgreen", "92D050"}};
    if (auto it = names.find(util::lower(s)); it != names.end()) return it->second;
    if (s.size() != 6) throw std::runtime_error("color must be RRGGBB hex, 'auto', or a basic name: " + s);
    for (auto& c : s) {
        if (!std::isxdigit((unsigned char)c)) throw std::runtime_error("bad color " + s);
        c = char(std::toupper((unsigned char)c));
    }
    return s;
}

bool truthy(const Json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) {
        auto s = util::lower(v.get<std::string>());
        return !(s == "false" || s == "0" || s == "off" || s == "no" || s.empty());
    }
    return !v.is_null();
}

// Toggle element: on -> <w:b/>, off -> <w:b w:val="0"/>, null -> remove.
void onoff(pugi::xml_node pr, const char* q, const Json& v) {
    if (v.is_null()) { pr.remove_child(q); return; }
    auto e = schema::ensure_child(pr, q);
    if (truthy(v)) xml::remove_attr(e, "w:val");
    else xml::set_attr(e, "w:val", "0");
}

void val(pugi::xml_node pr, const char* q, const Json& v, const std::string& s) {
    if (v.is_null()) { pr.remove_child(q); return; }
    xml::set_attr(schema::ensure_child(pr, q), "w:val", s);
}

std::string str(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        double d = v.get<double>();
        if (d == std::floor(d)) return std::to_string((long long)d);
        return std::to_string(d);
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();
}

void check_enum(pugi::xml_node e, const char* attr, const std::string& v) {
    std::string why;
    if (!schema::value_valid(schema::type_of(e), attr, v, &why))
        throw std::runtime_error(std::string(e.name()) + "/@" + attr + ": " + why);
}

void border(pugi::xml_node b, const Json& v) {
    // "single" | {"style":"single","size":4,"color":"auto","space":0}
    std::string style = "single";
    Json o = v.is_object() ? v : Json::object();
    if (v.is_string()) style = v.get<std::string>();
    if (v.is_boolean()) style = v.get<bool>() ? "single" : "none";
    if (o.contains("style")) style = o["style"].get<std::string>();
    check_enum(b, "w:val", style);
    xml::set_attr(b, "w:val", style);
    if (style != "none" && style != "nil") {
        // size in points -> eighths
        double sz = o.contains("size") ? o["size"].get<double>() : 0.5;
        xml::set_attr(b, "w:sz", std::to_string(std::lround(sz * 8)));
        xml::set_attr(b, "w:space", o.contains("space") ? str(o["space"]) : "0");
        xml::set_attr(b, "w:color", o.contains("color") ? hexcolor(o["color"]) : "auto");
    }
}

}  // namespace

void apply_rpr_json(Document& d, pugi::xml_node rpr, const Json& props) {
    if (!props.is_object()) throw std::runtime_error("run properties must be an object");
    for (auto& [k, v] : props.items()) {
        if (k == "style") {
            if (v.is_null()) rpr.remove_child("w:rStyle");
            else val(rpr, "w:rStyle", v, v.get<std::string>());
        } else if (k == "bold") { onoff(rpr, "w:b", v); onoff(rpr, "w:bCs", v); }
        else if (k == "italic") { onoff(rpr, "w:i", v); onoff(rpr, "w:iCs", v); }
        else if (k == "caps" || k == "all_caps") onoff(rpr, "w:caps", v);
        else if (k == "small_caps") onoff(rpr, "w:smallCaps", v);
        else if (k == "strike" || k == "strikethrough") onoff(rpr, "w:strike", v);
        else if (k == "double_strike") onoff(rpr, "w:dstrike", v);
        else if (k == "outline") onoff(rpr, "w:outline", v);
        else if (k == "shadow") onoff(rpr, "w:shadow", v);
        else if (k == "emboss") onoff(rpr, "w:emboss", v);
        else if (k == "engrave" || k == "imprint") onoff(rpr, "w:imprint", v);
        else if (k == "no_proof") onoff(rpr, "w:noProof", v);
        else if (k == "hidden") onoff(rpr, "w:vanish", v);
        else if (k == "web_hidden") onoff(rpr, "w:webHidden", v);
        else if (k == "rtl") onoff(rpr, "w:rtl", v);
        else if (k == "complex_script") onoff(rpr, "w:cs", v);
        else if (k == "snap_to_grid") onoff(rpr, "w:snapToGrid", v);
        else if (k == "spec_vanish") onoff(rpr, "w:specVanish", v);
        else if (k == "math") onoff(rpr, "w:oMath", v);
        else if (k == "font" || k == "font_name") {
            if (v.is_null()) { rpr.remove_child("w:rFonts"); continue; }
            auto f = schema::ensure_child(rpr, "w:rFonts");
            for (auto a : {"w:asciiTheme", "w:hAnsiTheme", "w:eastAsiaTheme", "w:cstheme"}) xml::remove_attr(f, a);
            if (v.is_string()) {
                std::string n = v.get<std::string>();
                xml::set_attr(f, "w:ascii", n);
                xml::set_attr(f, "w:hAnsi", n);
                xml::set_attr(f, "w:cs", n);
                xml::set_attr(f, "w:eastAsia", n);
            } else {
                for (auto& [fk, fv] : v.items()) {
                    static const std::map<std::string, std::string> m = {
                        {"ascii", "w:ascii"}, {"h_ansi", "w:hAnsi"}, {"east_asia", "w:eastAsia"}, {"cs", "w:cs"},
                        {"ascii_theme", "w:asciiTheme"}, {"h_ansi_theme", "w:hAnsiTheme"},
                        {"east_asia_theme", "w:eastAsiaTheme"}, {"cs_theme", "w:cstheme"}, {"hint", "w:hint"}};
                    auto it = m.find(fk);
                    if (it == m.end()) throw std::runtime_error("unknown font key " + fk);
                    if (fv.is_null()) xml::remove_attr(f, it->second);
                    else xml::set_attr(f, it->second, fv.get<std::string>());
                }
            }
        } else if (k == "size" || k == "font_size") {
            if (v.is_null()) { rpr.remove_child("w:sz"); rpr.remove_child("w:szCs"); continue; }
            long hp = std::lround(v.get<double>() * 2);
            val(rpr, "w:sz", v, std::to_string(hp));
            val(rpr, "w:szCs", v, std::to_string(hp));
        } else if (k == "color") {
            if (v.is_null()) { rpr.remove_child("w:color"); continue; }
            auto c = schema::ensure_child(rpr, "w:color");
            if (v.is_object()) {
                if (v.contains("theme")) xml::set_attr(c, "w:themeColor", v["theme"].get<std::string>());
                xml::set_attr(c, "w:val", v.contains("rgb") ? hexcolor(v["rgb"]) : "auto");
                if (v.contains("tint")) xml::set_attr(c, "w:themeTint", v["tint"].get<std::string>());
                if (v.contains("shade")) xml::set_attr(c, "w:themeShade", v["shade"].get<std::string>());
            } else {
                xml::set_attr(c, "w:val", hexcolor(v));
                xml::remove_attr(c, "w:themeColor");
                xml::remove_attr(c, "w:themeTint");
                xml::remove_attr(c, "w:themeShade");
            }
        } else if (k == "highlight") {
            if (v.is_null() || (v.is_string() && v.get<std::string>() == "none")) { rpr.remove_child("w:highlight"); continue; }
            auto h = schema::ensure_child(rpr, "w:highlight");
            check_enum(h, "w:val", v.get<std::string>());
            xml::set_attr(h, "w:val", v.get<std::string>());
        } else if (k == "underline") {
            if (v.is_null()) { rpr.remove_child("w:u"); continue; }
            std::string u = v.is_boolean() ? (v.get<bool>() ? "single" : "none") : v.is_object() ? v.value("style", "single") : v.get<std::string>();
            auto e = schema::ensure_child(rpr, "w:u");
            check_enum(e, "w:val", u);
            xml::set_attr(e, "w:val", u);
            if (v.is_object() && v.contains("color")) xml::set_attr(e, "w:color", hexcolor(v["color"]));
        } else if (k == "vertical_align" || k == "superscript" || k == "subscript") {
            std::string a = k == "superscript" ? (truthy(v) ? "superscript" : "baseline")
                          : k == "subscript" ? (truthy(v) ? "subscript" : "baseline")
                          : v.is_null() ? "" : v.get<std::string>();
            if (a.empty() || a == "baseline") { rpr.remove_child("w:vertAlign"); continue; }
            auto e = schema::ensure_child(rpr, "w:vertAlign");
            check_enum(e, "w:val", a);
            xml::set_attr(e, "w:val", a);
        } else if (k == "spacing" || k == "character_spacing") {
            if (v.is_null()) { rpr.remove_child("w:spacing"); continue; }
            val(rpr, "w:spacing", v, std::to_string(twips(v)));
        } else if (k == "scale") {
            val(rpr, "w:w", v, str(v));
        } else if (k == "kerning") {
            if (v.is_null()) { rpr.remove_child("w:kern"); continue; }
            val(rpr, "w:kern", v, std::to_string(std::lround(v.get<double>() * 2)));
        } else if (k == "position") {
            if (v.is_null()) { rpr.remove_child("w:position"); continue; }
            val(rpr, "w:position", v, std::to_string(std::lround(v.get<double>() * 2)));
        } else if (k == "shading") {
            if (v.is_null()) { rpr.remove_child("w:shd"); continue; }
            auto s = schema::ensure_child(rpr, "w:shd");
            xml::set_attr(s, "w:val", v.is_object() ? v.value("pattern", "clear") : "clear");
            xml::set_attr(s, "w:color", v.is_object() && v.contains("color") ? hexcolor(v["color"]) : "auto");
            xml::set_attr(s, "w:fill", hexcolor(v.is_object() ? v["fill"] : v));
        } else if (k == "border") {
            if (v.is_null()) { rpr.remove_child("w:bdr"); continue; }
            border(schema::ensure_child(rpr, "w:bdr"), v);
        } else if (k == "language" || k == "lang") {
            if (v.is_null()) { rpr.remove_child("w:lang"); continue; }
            auto l = schema::ensure_child(rpr, "w:lang");
            if (v.is_string()) xml::set_attr(l, "w:val", v.get<std::string>());
            else {
                if (v.contains("latin")) xml::set_attr(l, "w:val", v["latin"].get<std::string>());
                if (v.contains("east_asia")) xml::set_attr(l, "w:eastAsia", v["east_asia"].get<std::string>());
                if (v.contains("bidi")) xml::set_attr(l, "w:bidi", v["bidi"].get<std::string>());
            }
        } else if (k == "emphasis_mark") {
            val(rpr, "w:em", v, v.is_null() ? "" : v.get<std::string>());
        } else if (k == "effect") {
            val(rpr, "w:effect", v, v.is_null() ? "" : v.get<std::string>());
        } else if (k == "xml") {
            // raw rPr children, e.g. "<w14:ligatures w14:val=\"standard\"/>"
            pugi::xml_document frag;
            std::string err;
            if (!xml::parse_fragment(v.get<std::string>(), frag, &err)) throw std::runtime_error("bad xml: " + err);
            for (auto c : frag.children()) {
                rpr.remove_child(c.name());
                auto n = rpr.append_copy(c);
                schema::place_child(rpr, n);
            }
        } else {
            throw std::runtime_error("unknown run property '" + k + "'");
        }
    }
    xml::fix_declarations(xml::root_of(rpr));
}

void apply_ppr_json(Document& d, pugi::xml_node ppr, const Json& props) {
    if (!props.is_object()) throw std::runtime_error("paragraph properties must be an object");
    for (auto& [k, v] : props.items()) {
        if (k == "style") {
            if (v.is_null()) ppr.remove_child("w:pStyle");
            else val(ppr, "w:pStyle", v, v.get<std::string>());
        } else if (k == "alignment" || k == "align" || k == "justification") {
            if (v.is_null()) { ppr.remove_child("w:jc"); continue; }
            std::string a = v.get<std::string>();
            if (a == "justify" || a == "justified") a = "both";
            auto e = schema::ensure_child(ppr, "w:jc");
            check_enum(e, "w:val", a);
            xml::set_attr(e, "w:val", a);
        } else if (k == "keep_with_next") onoff(ppr, "w:keepNext", v);
        else if (k == "keep_together" || k == "keep_lines") onoff(ppr, "w:keepLines", v);
        else if (k == "page_break_before") onoff(ppr, "w:pageBreakBefore", v);
        else if (k == "widow_control") onoff(ppr, "w:widowControl", v);
        else if (k == "suppress_line_numbers") onoff(ppr, "w:suppressLineNumbers", v);
        else if (k == "suppress_auto_hyphens") onoff(ppr, "w:suppressAutoHyphens", v);
        else if (k == "contextual_spacing") onoff(ppr, "w:contextualSpacing", v);
        else if (k == "mirror_indents") onoff(ppr, "w:mirrorIndents", v);
        else if (k == "bidi" || k == "rtl") onoff(ppr, "w:bidi", v);
        else if (k == "outline_level") {
            if (v.is_null()) { ppr.remove_child("w:outlineLvl"); continue; }
            val(ppr, "w:outlineLvl", v, std::to_string(v.get<int>() - 1));  // 1-based in, 0-based out
        } else if (k == "space_before" || k == "space_after" || k == "line_spacing" || k == "line_rule" ||
                   k == "space_before_auto" || k == "space_after_auto") {
            auto s = schema::ensure_child(ppr, "w:spacing");
            if (k == "space_before") { if (v.is_null()) xml::remove_attr(s, "w:before"); else xml::set_attr(s, "w:before", std::to_string(twips(v))); }
            if (k == "space_after") { if (v.is_null()) xml::remove_attr(s, "w:after"); else xml::set_attr(s, "w:after", std::to_string(twips(v))); }
            if (k == "space_before_auto") xml::set_attr(s, "w:beforeAutospacing", truthy(v) ? "1" : "0");
            if (k == "space_after_auto") xml::set_attr(s, "w:afterAutospacing", truthy(v) ? "1" : "0");
            if (k == "line_spacing") {
                // number = multiple of single (1.15, 1.5, 2); "12pt" = exact/at least
                if (v.is_null()) { xml::remove_attr(s, "w:line"); xml::remove_attr(s, "w:lineRule"); }
                else if (v.is_number()) {
                    xml::set_attr(s, "w:line", std::to_string(std::lround(v.get<double>() * 240)));
                    xml::set_attr(s, "w:lineRule", "auto");
                } else {
                    xml::set_attr(s, "w:line", std::to_string(twips(v)));
                    if (xml::attr(s, "w:lineRule").empty() || xml::attr(s, "w:lineRule") == "auto")
                        xml::set_attr(s, "w:lineRule", "exact");
                }
            }
            if (k == "line_rule") {
                std::string r = v.get<std::string>();
                if (r == "at_least") r = "atLeast";
                check_enum(s, "w:lineRule", r);
                xml::set_attr(s, "w:lineRule", r);
            }
            if (!s.first_attribute()) ppr.remove_child(s);
        } else if (k == "indent_left" || k == "indent_right" || k == "first_line_indent" || k == "hanging_indent" ||
                   k == "left_indent" || k == "right_indent") {
            auto ind = schema::ensure_child(ppr, "w:ind");
            if (k == "indent_left" || k == "left_indent") { if (v.is_null()) xml::remove_attr(ind, "w:left"); else xml::set_attr(ind, "w:left", std::to_string(twips(v))); }
            if (k == "indent_right" || k == "right_indent") { if (v.is_null()) xml::remove_attr(ind, "w:right"); else xml::set_attr(ind, "w:right", std::to_string(twips(v))); }
            if (k == "first_line_indent") {
                xml::remove_attr(ind, "w:hanging");
                if (v.is_null()) xml::remove_attr(ind, "w:firstLine");
                else {
                    long t = twips(v);
                    if (t < 0) { xml::remove_attr(ind, "w:firstLine"); xml::set_attr(ind, "w:hanging", std::to_string(-t)); }
                    else xml::set_attr(ind, "w:firstLine", std::to_string(t));
                }
            }
            if (k == "hanging_indent") {
                xml::remove_attr(ind, "w:firstLine");
                if (v.is_null()) xml::remove_attr(ind, "w:hanging");
                else xml::set_attr(ind, "w:hanging", std::to_string(twips(v)));
            }
            if (!ind.first_attribute()) ppr.remove_child(ind);
        } else if (k == "shading") {
            if (v.is_null()) { ppr.remove_child("w:shd"); continue; }
            auto s = schema::ensure_child(ppr, "w:shd");
            xml::set_attr(s, "w:val", "clear");
            xml::set_attr(s, "w:color", "auto");
            xml::set_attr(s, "w:fill", hexcolor(v.is_object() ? v["fill"] : v));
        } else if (k == "borders") {
            if (v.is_null()) { ppr.remove_child("w:pBdr"); continue; }
            auto b = schema::ensure_child(ppr, "w:pBdr");
            for (auto& [side, bv] : v.items()) {
                static const std::set<std::string> sides = {"top", "left", "bottom", "right", "between", "bar"};
                if (!sides.count(side)) throw std::runtime_error("unknown border side " + side);
                std::string q = "w:" + side;
                if (bv.is_null()) { b.remove_child(q.c_str()); continue; }
                border(schema::ensure_child(b, q), bv);
            }
        } else if (k == "tabs") {
            if (v.is_null()) { ppr.remove_child("w:tabs"); continue; }
            ppr.remove_child("w:tabs");
            auto tabs = schema::ensure_child(ppr, "w:tabs");
            for (auto& t : v) {
                auto e = tabs.append_child("w:tab");
                std::string align = t.value("align", "left");
                check_enum(e, "w:val", align);
                xml::set_attr(e, "w:val", align);
                if (t.contains("leader")) xml::set_attr(e, "w:leader", t["leader"].get<std::string>());
                xml::set_attr(e, "w:pos", std::to_string(twips(t["pos"])));
            }
        } else if (k == "numbering" || k == "list") {
            // {"num_id":3,"level":0} or null to remove
            if (v.is_null()) {
                // removing explicit numbering when the style numbers: numId 0
                ppr.remove_child("w:numPr");
                continue;
            }
            auto np = schema::ensure_child(ppr, "w:numPr");
            xml::set_attr(schema::ensure_child(np, "w:ilvl"), "w:val", std::to_string(v.value("level", 0)));
            xml::set_attr(schema::ensure_child(np, "w:numId"), "w:val", std::to_string(v.at("num_id").get<int>()));
        } else if (k == "frame") {
            if (v.is_null()) { ppr.remove_child("w:framePr"); continue; }
            auto f = schema::ensure_child(ppr, "w:framePr");
            for (auto& [fk, fv] : v.items()) xml::set_attr(f, "w:" + fk, str(fv));
        } else if (k == "text_direction") {
            val(ppr, "w:textDirection", v, v.is_null() ? "" : v.get<std::string>());
        } else if (k == "text_alignment") {
            val(ppr, "w:textAlignment", v, v.is_null() ? "" : v.get<std::string>());
        } else if (k == "xml") {
            pugi::xml_document frag;
            std::string err;
            if (!xml::parse_fragment(v.get<std::string>(), frag, &err)) throw std::runtime_error("bad xml: " + err);
            for (auto c : frag.children()) {
                ppr.remove_child(c.name());
                schema::place_child(ppr, ppr.append_copy(c));
            }
        } else {
            throw std::runtime_error("unknown paragraph property '" + k + "'");
        }
    }
    xml::fix_declarations(xml::root_of(ppr));
}

namespace {
std::optional<bool> on(pugi::xml_node pr, const char* q) {
    auto e = pr.child(q);
    if (!e) return std::nullopt;
    std::string v = xml::attr(e, "w:val", "true");
    return !(v == "0" || v == "false" || v == "off");
}
double pts(const std::string& tw) { return tw.empty() ? 0 : std::stod(tw) / 20.0; }
}  // namespace

Json rpr_to_json(pugi::xml_node r) {
    Json j = Json::object();
    if (!r) return j;
    if (auto s = r.child("w:rStyle")) j["style"] = xml::attr(s, "w:val");
    static const std::pair<const char*, const char*> toggles[] = {
        {"w:b", "bold"}, {"w:i", "italic"}, {"w:caps", "caps"}, {"w:smallCaps", "small_caps"},
        {"w:strike", "strike"}, {"w:dstrike", "double_strike"}, {"w:outline", "outline"}, {"w:shadow", "shadow"},
        {"w:emboss", "emboss"}, {"w:imprint", "engrave"}, {"w:noProof", "no_proof"}, {"w:vanish", "hidden"},
        {"w:rtl", "rtl"}, {"w:webHidden", "web_hidden"}};
    for (auto& [q, n] : toggles) if (auto v = on(r, q)) j[n] = *v;
    if (auto f = r.child("w:rFonts")) {
        std::string a = xml::attr(f, "w:ascii");
        if (!a.empty()) j["font"] = a;
        else if (!xml::attr(f, "w:asciiTheme").empty()) j["font"] = Json{{"ascii_theme", xml::attr(f, "w:asciiTheme")}};
    }
    if (auto s = r.child("w:sz")) j["size"] = std::stod(xml::attr(s, "w:val", "0")) / 2.0;
    if (auto c = r.child("w:color")) {
        if (!xml::attr(c, "w:themeColor").empty()) j["color"] = Json{{"theme", xml::attr(c, "w:themeColor")}, {"rgb", xml::attr(c, "w:val")}};
        else j["color"] = xml::attr(c, "w:val");
    }
    if (auto h = r.child("w:highlight")) j["highlight"] = xml::attr(h, "w:val");
    if (auto u = r.child("w:u")) j["underline"] = xml::attr(u, "w:val");
    if (auto v = r.child("w:vertAlign")) j["vertical_align"] = xml::attr(v, "w:val");
    if (auto s = r.child("w:spacing")) j["spacing"] = pts(xml::attr(s, "w:val"));
    if (auto s = r.child("w:shd")) j["shading"] = xml::attr(s, "w:fill");
    if (auto l = r.child("w:lang")) j["language"] = xml::attr(l, "w:val");
    return j;
}

Json ppr_to_json(pugi::xml_node p) {
    Json j = Json::object();
    if (!p) return j;
    if (auto s = p.child("w:pStyle")) j["style"] = xml::attr(s, "w:val");
    if (auto s = p.child("w:jc")) j["alignment"] = xml::attr(s, "w:val");
    static const std::pair<const char*, const char*> toggles[] = {
        {"w:keepNext", "keep_with_next"}, {"w:keepLines", "keep_together"}, {"w:pageBreakBefore", "page_break_before"},
        {"w:widowControl", "widow_control"}, {"w:contextualSpacing", "contextual_spacing"}, {"w:bidi", "bidi"}};
    for (auto& [q, n] : toggles) if (auto v = on(p, q)) j[n] = *v;
    if (auto s = p.child("w:spacing")) {
        if (s.attribute("w:before")) j["space_before"] = pts(xml::attr(s, "w:before"));
        if (s.attribute("w:after")) j["space_after"] = pts(xml::attr(s, "w:after"));
        if (s.attribute("w:line")) {
            std::string rule = xml::attr(s, "w:lineRule", "auto");
            double line = std::stod(xml::attr(s, "w:line"));
            if (rule == "auto") j["line_spacing"] = line / 240.0;
            else { j["line_spacing"] = std::to_string(line / 20.0) + "pt"; j["line_rule"] = rule; }
        }
    }
    if (auto i = p.child("w:ind")) {
        auto get = [&](const char* a, const char* b) { std::string v = xml::attr(i, a); return v.empty() ? xml::attr(i, b) : v; };
        if (auto v = get("w:left", "w:start"); !v.empty()) j["indent_left"] = pts(v);
        if (auto v = get("w:right", "w:end"); !v.empty()) j["indent_right"] = pts(v);
        if (auto v = xml::attr(i, "w:firstLine"); !v.empty()) j["first_line_indent"] = pts(v);
        if (auto v = xml::attr(i, "w:hanging"); !v.empty()) j["hanging_indent"] = pts(v);
    }
    if (auto np = p.child("w:numPr")) {
        j["numbering"] = {{"num_id", std::stoi(xml::attr(np.child("w:numId"), "w:val", "0"))},
                          {"level", std::stoi(xml::attr(np.child("w:ilvl"), "w:val", "0"))}};
    }
    if (auto o = p.child("w:outlineLvl")) j["outline_level"] = std::stoi(xml::attr(o, "w:val", "9")) + 1;
    if (auto s = p.child("w:shd")) j["shading"] = xml::attr(s, "w:fill");
    return j;
}

}  // namespace docx::wml
