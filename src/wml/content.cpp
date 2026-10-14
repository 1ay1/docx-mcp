// Pictures, breaks, sections, headers/footers, notes, bookmarks, hyperlinks.
#include <map>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "xml/xml.hpp"

namespace docx::wml {

namespace {

bool tracking(Document& d, const EditOpts& o) { return o.track ? *o.track : d.track_changes(); }

long twips_of(const Json& v) { return util::length_twips(v); }

// Insert a run (already built, detached under a holder) at visible pos.
pugi::xml_node place_run(Document& d, pugi::xml_node p, int pos, pugi::xml_node run_src, const EditOpts& o) {
    split_at(p, pos);
    ParaText pt = analyze(p);
    int n = (int)pt.vis.size();
    pugi::xml_node parent = p, before;
    if (pos < n) {
        auto node = pt.atoms[pt.vis[pos]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        parent = node.parent();
        before = node;
    } else if (n > 0) {
        auto node = pt.atoms[pt.vis[n - 1]].run;
        while (is_revision_container(node.parent())) node = node.parent();
        parent = node.parent();
        before = node.next_sibling();
    }
    pugi::xml_node r = before ? parent.insert_copy_before(run_src, before) : parent.append_copy(run_src);
    if (tracking(d, o)) {
        auto ins = parent.insert_child_before("w:ins", r);
        stamp(d, ins);
        ins.append_move(r);
    }
    return r;
}

// ── image sniffing ──
struct ImgInfo { std::string ext, ctype; long w = 0, h = 0; double dpi_x = 96, dpi_y = 96; };

std::uint32_t be32(const std::uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

ImgInfo sniff(const util::Bytes& b) {
    ImgInfo i;
    const auto* p = b.data();
    std::size_t n = b.size();
    if (n > 24 && !std::memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        i.ext = "png"; i.ctype = "image/png";
        i.w = be32(p + 16); i.h = be32(p + 20);
        // pHYs chunk for DPI
        for (std::size_t off = 8; off + 12 <= n;) {
            std::uint32_t len = be32(p + off);
            if (!std::memcmp(p + off + 4, "pHYs", 4) && off + 8 + 9 <= n) {
                std::uint32_t px = be32(p + off + 8), py = be32(p + off + 12);
                if (p[off + 16] == 1 && px && py) { i.dpi_x = px * 0.0254; i.dpi_y = py * 0.0254; }
                break;
            }
            if (!std::memcmp(p + off + 4, "IDAT", 4)) break;
            off += 12 + len;
        }
        return i;
    }
    if (n > 4 && p[0] == 0xFF && p[1] == 0xD8) {
        i.ext = "jpeg"; i.ctype = "image/jpeg";
        for (std::size_t off = 2; off + 9 < n;) {
            if (p[off] != 0xFF) { off++; continue; }
            std::uint8_t m = p[off + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { off += 2; continue; }
            std::uint16_t len = be16(p + off + 2);
            if (m == 0xE0 && off + 18 < n && !std::memcmp(p + off + 4, "JFIF", 4)) {
                std::uint8_t units = p[off + 11];
                std::uint16_t dx = be16(p + off + 12), dy = be16(p + off + 14);
                if (units == 1 && dx && dy) { i.dpi_x = dx; i.dpi_y = dy; }
                if (units == 2 && dx && dy) { i.dpi_x = dx * 2.54; i.dpi_y = dy * 2.54; }
            }
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                i.h = be16(p + off + 5); i.w = be16(p + off + 7);
                return i;
            }
            off += 2 + len;
        }
        return i;
    }
    if (n > 10 && (!std::memcmp(p, "GIF87a", 6) || !std::memcmp(p, "GIF89a", 6))) {
        i.ext = "gif"; i.ctype = "image/gif"; i.w = le16(p + 6); i.h = le16(p + 8);
        return i;
    }
    if (n > 26 && p[0] == 'B' && p[1] == 'M') {
        i.ext = "bmp"; i.ctype = "image/bmp"; i.w = (long)le32(p + 18); i.h = std::labs((long)(std::int32_t)le32(p + 22));
        return i;
    }
    if (n > 8 && ((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M'))) {
        i.ext = "tiff"; i.ctype = "image/tiff";
        bool le = p[0] == 'I';
        auto u16 = [&](std::size_t o) { return le ? le16(p + o) : be16(p + o); };
        auto u32 = [&](std::size_t o) { return le ? le32(p + o) : be32(p + o); };
        std::size_t ifd = u32(4);
        if (ifd + 2 < n) {
            int cnt = u16(ifd);
            for (int k = 0; k < cnt && ifd + 2 + k * 12 + 12 <= n; k++) {
                std::size_t e = ifd + 2 + k * 12;
                int tag = u16(e), type = u16(e + 2);
                long val = type == 3 ? u16(e + 8) : (long)u32(e + 8);
                if (tag == 256) i.w = val;
                if (tag == 257) i.h = val;
            }
        }
        return i;
    }
    if (n > 44 && le32(p) == 1 && le32(p + 40) == 0x464D4520) {
        i.ext = "emf"; i.ctype = "image/x-emf";
        // rclFrame in .01 mm
        long fw = (long)(std::int32_t)le32(p + 32) - (long)(std::int32_t)le32(p + 24);
        long fh = (long)(std::int32_t)le32(p + 36) - (long)(std::int32_t)le32(p + 28);
        i.w = std::lround(fw / 2540.0 * 96); i.h = std::lround(fh / 2540.0 * 96);
        return i;
    }
    if (n > 22 && le32(p) == 0x9AC6CDD7) {
        i.ext = "wmf"; i.ctype = "image/x-wmf";
        long l = (std::int16_t)le16(p + 6), t = (std::int16_t)le16(p + 8), r = (std::int16_t)le16(p + 10), bo = (std::int16_t)le16(p + 12);
        long inch = le16(p + 14);
        if (inch) { i.w = std::lround((r - l) * 96.0 / inch); i.h = std::lround((bo - t) * 96.0 / inch); }
        return i;
    }
    std::string head(reinterpret_cast<const char*>(p), std::min<std::size_t>(n, 512));
    if (head.find("<svg") != std::string::npos || head.find("<?xml") != std::string::npos) {
        i.ext = "svg"; i.ctype = "image/svg+xml";
        std::string s(reinterpret_cast<const char*>(p), n);
        auto num = [&](const char* a) -> long {
            auto k = s.find(std::string(a) + "=\"");
            if (k == std::string::npos) return 0;
            return std::lround(std::atof(s.c_str() + k + std::strlen(a) + 2));
        };
        i.w = num("width"); i.h = num("height");
        if (!i.w || !i.h) { i.w = 300; i.h = 150; }
        return i;
    }
    throw std::runtime_error("unsupported image format (PNG, JPEG, GIF, BMP, TIFF, EMF, WMF, SVG)");
}

int next_docpr_id(Document& d) {
    int mx = 0;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        for (auto n : d.pkg().xml(part).select_nodes("//wp:docPr/@id")) mx = std::max(mx, n.attribute().as_int());
        for (auto n : d.pkg().xml(part).select_nodes("//pic:cNvPr/@id")) mx = std::max(mx, n.attribute().as_int());
    }
    return mx + 1;
}

}  // namespace

// ── pictures ─────────────────────────────────────────────────────────────

void make_floating(Document& d, pugi::xml_node inl, const Placement& pl);

pugi::xml_node insert_picture(Document& d, pugi::xml_node p, int pos, const util::Bytes& data, double wpt, double hpt,
                              const std::string& alt, const std::string& part_in, const EditOpts& o, const Placement& place) {
    ImgInfo im = sniff(data);
    std::string part = part_in.empty() ? d.main() : part_in;
    std::string ext = im.ext == "jpeg" ? "jpeg" : im.ext;
    std::string name = d.pkg().unique_name("/word/media/image", "." + ext);
    d.pkg().add_part(name, im.ctype, data);
    d.pkg().defaults.try_emplace(ext, im.ctype);
    d.pkg().set_override(name, im.ctype);
    std::string rid = d.pkg().rels(part).add(opc::rt::image, opc::Package::relative(part, name));
    // natural size in EMU (914400 per inch)
    double nat_w = im.w / im.dpi_x * 914400.0, nat_h = im.h / im.dpi_y * 914400.0;
    double cx, cy;
    if (wpt > 0 && hpt > 0) { cx = wpt * 12700; cy = hpt * 12700; }
    else if (wpt > 0) { cx = wpt * 12700; cy = nat_h * (cx / std::max(1.0, nat_w)); }
    else if (hpt > 0) { cy = hpt * 12700; cx = nat_w * (cy / std::max(1.0, nat_h)); }
    else { cx = nat_w; cy = nat_h; }
    // Word scales a too-wide picture down to the text width
    long tw_twips = 9360;
    if (auto sp = d.body().child("w:sectPr")) {
        long w = util::to_long(xml::attr(sp.child("w:pgSz"), "w:w", "12240"));
        long l = util::to_long(xml::attr(sp.child("w:pgMar"), "w:left", "1440"));
        long r = util::to_long(xml::attr(sp.child("w:pgMar"), "w:right", "1440"));
        tw_twips = w - l - r;
    }
    double max_cx = tw_twips * 635.0;
    if (wpt <= 0 && hpt <= 0 && cx > max_cx) { cy = cy * max_cx / cx; cx = max_cx; }
    long CX = std::lround(cx), CY = std::lround(cy);
    int id = next_docpr_id(d);
    std::string nm = "Picture " + std::to_string(id);
    pugi::xml_document tmp;
    auto r = tmp.append_child("w:r");
    auto drw = r.append_child("w:drawing");
    auto inl = drw.append_child("wp:inline");
    for (auto a : {"distT", "distB", "distL", "distR"}) inl.append_attribute(a) = "0";
    inl.append_attribute("wp14:anchorId") = d.new_hex_id().c_str();
    inl.append_attribute("wp14:editId") = d.new_hex_id().c_str();
    auto ext_e = inl.append_child("wp:extent");
    ext_e.append_attribute("cx") = std::to_string(CX).c_str();
    ext_e.append_attribute("cy") = std::to_string(CY).c_str();
    auto eff = inl.append_child("wp:effectExtent");
    for (auto a : {"l", "t", "r", "b"}) eff.append_attribute(a) = "0";
    auto dp = inl.append_child("wp:docPr");
    dp.append_attribute("id") = id;
    dp.append_attribute("name") = nm.c_str();
    if (!alt.empty()) dp.append_attribute("descr") = util::xml_safe(alt).c_str();
    auto gfp = inl.append_child("wp:cNvGraphicFramePr");
    gfp.append_child("a:graphicFrameLocks").append_attribute("noChangeAspect") = "1";
    auto gr = inl.append_child("a:graphic");
    auto gd = gr.append_child("a:graphicData");
    gd.append_attribute("uri") = "http://schemas.openxmlformats.org/drawingml/2006/picture";
    auto pic = gd.append_child("pic:pic");
    auto nv = pic.append_child("pic:nvPicPr");
    auto cnv = nv.append_child("pic:cNvPr");
    cnv.append_attribute("id") = "0";
    cnv.append_attribute("name") = (nm + "." + ext).c_str();
    if (!alt.empty()) cnv.append_attribute("descr") = util::xml_safe(alt).c_str();
    nv.append_child("pic:cNvPicPr");
    auto bf = pic.append_child("pic:blipFill");
    auto blip = bf.append_child("a:blip");
    blip.append_attribute("r:embed") = rid.c_str();
    bf.append_child("a:stretch").append_child("a:fillRect");
    auto sp = pic.append_child("pic:spPr");
    auto xf = sp.append_child("a:xfrm");
    auto off = xf.append_child("a:off");
    off.append_attribute("x") = "0";
    off.append_attribute("y") = "0";
    auto ex2 = xf.append_child("a:ext");
    ex2.append_attribute("cx") = std::to_string(CX).c_str();
    ex2.append_attribute("cy") = std::to_string(CY).c_str();
    auto geom = sp.append_child("a:prstGeom");
    geom.append_attribute("prst") = "rect";
    geom.append_child("a:avLst");
    if (place.wrap != "inline") make_floating(d, inl, place);
    auto out = place_run(d, p, pos, r, o);
    auto root = xml::root_of(p);
    for (auto pre : {"wp", "a", "pic", "r"}) xml::declare(root, pre);
    xml::declare(root, "wp14", true);
    return out;
}

namespace {
long emu(double pt) { return std::lround(pt * 12700); }

void set_wrap(pugi::xml_node anchor, const Placement& pl) {
    // the wrap element sits after effectExtent, before docPr
    for (auto q : {"wp:wrapNone", "wp:wrapSquare", "wp:wrapTight", "wp:wrapThrough", "wp:wrapTopAndBottom"})
        anchor.remove_child(q);
    auto before = anchor.child("wp:docPr");
    std::string w = pl.wrap;
    pugi::xml_node e;
    if (w == "behind" || w == "in_front") e = anchor.insert_child_before("wp:wrapNone", before);
    else if (w == "top_bottom") e = anchor.insert_child_before("wp:wrapTopAndBottom", before);
    else {
        e = anchor.insert_child_before(w == "tight" ? "wp:wrapTight" : w == "through" ? "wp:wrapThrough" : "wp:wrapSquare", before);
        e.append_attribute("wrapText") = "bothSides";
        if (w == "tight" || w == "through") {
            // Word's default contour for a picture: its rectangle, in 1/21600ths
            auto poly = e.append_child("wp:wrapPolygon");
            poly.append_attribute("edited") = "0";
            auto pt = [&](const char* q, const char* x, const char* y) {
                auto n = poly.append_child(q);
                n.append_attribute("x") = x;
                n.append_attribute("y") = y;
            };
            pt("wp:start", "0", "0");
            pt("wp:lineTo", "0", "21600");
            pt("wp:lineTo", "21600", "21600");
            pt("wp:lineTo", "21600", "0");
            pt("wp:lineTo", "0", "0");
        }
    }
    xml::set_attr(anchor, "behindDoc", w == "behind" ? "1" : "0");
    long gap = emu(pl.distance_pt);
    bool tb = w == "top_bottom";
    xml::set_attr(anchor, "distT", std::to_string(tb ? gap : 0));
    xml::set_attr(anchor, "distB", std::to_string(tb ? gap : 0));
    xml::set_attr(anchor, "distL", std::to_string(tb || w == "behind" || w == "in_front" ? 0 : gap));
    xml::set_attr(anchor, "distR", std::to_string(tb || w == "behind" || w == "in_front" ? 0 : gap));
}

void set_position(pugi::xml_node anchor, const Placement& pl) {
    auto fill = [&](const char* q, const std::string& from, const std::string& align, double off) {
        auto e = anchor.child(q);
        while (e.first_child()) e.remove_child(e.first_child());
        xml::set_attr(e, "relativeFrom", from);
        if (!align.empty()) e.append_child("wp:align").text().set(align.c_str());
        else e.append_child("wp:posOffset").text().set(std::to_string(emu(off)).c_str());
    };
    fill("wp:positionH", pl.h_from, pl.h_align, pl.x_pt);
    fill("wp:positionV", pl.v_from, pl.v_align, pl.y_pt);
}
}  // namespace

Placement placement_from_json(const Json& j) {
    Placement pl;
    if (j.is_null() || (j.is_object() && j.empty())) return pl;
    if (j.is_string()) pl.wrap = j.get<std::string>();
    auto norm = [](std::string s) {
        s = util::lower(util::trim(s));
        for (auto& c : s) if (c == '-' || c == ' ') c = '_';
        return s;
    };
    if (j.is_object()) {
        for (auto& [k, v] : j.items()) {
            std::string sv = v.is_string() ? norm(v.get<std::string>()) : "";
            if (k == "wrap") pl.wrap = sv;
            else if (k == "x") pl.x_pt = v.is_number() ? v.get<double>() : util::length_twips(v) / 20.0;
            else if (k == "y") pl.y_pt = v.is_number() ? v.get<double>() : util::length_twips(v) / 20.0;
            else if (k == "horizontal") pl.h_align = sv;
            else if (k == "vertical") pl.v_align = sv;
            else if (k == "horizontal_from" || k == "relative_to_x") pl.h_from = sv;
            else if (k == "vertical_from" || k == "relative_to_y") pl.v_from = sv;
            else if (k == "distance") pl.distance_pt = v.is_number() ? v.get<double>() : util::length_twips(v) / 20.0;
            else
                throw std::runtime_error(util::unknown_key("placement key", k, {"wrap", "horizontal", "vertical", "x", "y",
                                                                              "horizontal_from", "vertical_from", "distance"}));
        }
    } else {
        pl.wrap = norm(pl.wrap);
    }
    // friendly names for Word's Layout Options
    static const std::map<std::string, std::string> wraps = {
        {"inline", "inline"}, {"in_line", "inline"}, {"in_line_with_text", "inline"}, {"square", "square"},
        {"tight", "tight"}, {"through", "through"}, {"top_bottom", "top_bottom"}, {"top_and_bottom", "top_bottom"},
        {"behind", "behind"}, {"behind_text", "behind"}, {"in_front", "in_front"}, {"in_front_of_text", "in_front"},
        {"front", "in_front"}};
    auto it = wraps.find(pl.wrap);
    if (it == wraps.end())
        throw std::runtime_error(util::unknown_key("wrap", pl.wrap, {"inline", "square", "tight", "through", "top_bottom",
                                                                    "behind", "in_front"}));
    pl.wrap = it->second;
    auto check = [](const std::string& v, std::initializer_list<std::string_view> ok, const char* what) {
        if (v.empty()) return;
        for (auto o : ok) if (v == o) return;
        throw std::runtime_error(util::unknown_key(what, v, std::vector<std::string_view>(ok)));
    };
    check(pl.h_align, {"left", "center", "right", "inside", "outside"}, "horizontal alignment");
    check(pl.v_align, {"top", "center", "bottom", "inside", "outside"}, "vertical alignment");
    check(pl.h_from, {"margin", "page", "column", "character", "leftMargin", "rightMargin"}, "horizontal_from");
    check(pl.v_from, {"margin", "page", "paragraph", "line", "topMargin", "bottomMargin"}, "vertical_from");
    if (pl.distance_pt < 0 || pl.distance_pt > 300) throw std::runtime_error("distance must be 0 to 300 points");
    return pl;
}

// Turn a wp:inline into a wp:anchor in place, keeping its graphic.
void make_floating(Document& d, pugi::xml_node inl, const Placement& pl) {
    auto drw = inl.parent();
    auto anchor = drw.insert_child_before("wp:anchor", inl);
    for (auto a : {"distT", "distB", "distL", "distR"}) anchor.append_attribute(a) = "0";
    anchor.append_attribute("simplePos") = "0";
    // stack order: above everything placed so far
    unsigned long z = 251658240;
    for (auto& part : d.story_parts())
        if (d.pkg().has(part))
            for (auto n : d.pkg().xml(part).select_nodes("//wp:anchor/@relativeHeight"))
                z = std::max<unsigned long>(z, n.attribute().as_ullong() + 1024);
    anchor.append_attribute("relativeHeight") = std::to_string(z).c_str();
    anchor.append_attribute("behindDoc") = "0";
    anchor.append_attribute("locked") = "0";
    anchor.append_attribute("layoutInCell") = "1";
    anchor.append_attribute("allowOverlap") = "1";
    for (auto a : {"wp14:anchorId", "wp14:editId"})
        if (auto v = inl.attribute(a)) anchor.append_attribute(a) = v.value();
    auto sp = anchor.append_child("wp:simplePos");
    sp.append_attribute("x") = "0";
    sp.append_attribute("y") = "0";
    anchor.append_child("wp:positionH");
    anchor.append_child("wp:positionV");
    for (auto c = inl.first_child(); c;) {
        auto nx = c.next_sibling();
        anchor.append_move(c);
        c = nx;
    }
    drw.remove_child(inl);
    if (!anchor.child("wp:effectExtent")) {
        auto ee = anchor.insert_child_after("wp:effectExtent", anchor.child("wp:extent"));
        for (auto a : {"l", "t", "r", "b"}) ee.append_attribute(a) = "0";
    }
    set_position(anchor, pl);
    set_wrap(anchor, pl);
}

namespace {
// Every picture drawing in body order: the wp:inline or wp:anchor holding a pic:pic.
std::vector<pugi::xml_node> picture_frames(Document& d) {
    std::vector<pugi::xml_node> v;
    for (auto drw : xml::descendants(d.body(), "w:drawing"))
        for (auto f : drw.children())
            if ((xml::name_is(f, "wp:inline") || xml::name_is(f, "wp:anchor")) && xml::first_descendant(f, "pic:pic"))
                v.push_back(f);
    return v;
}

pugi::xml_node frame_at(Document& d, int n) {
    auto v = picture_frames(d);
    if (n < 1 || n > (int)v.size())
        throw std::runtime_error("picture " + std::to_string(n) + " not found (document has " + std::to_string(v.size()) +
                                 "; list_pictures shows them)");
    return v[n - 1];
}

std::string wrap_of(pugi::xml_node f) {
    if (xml::name_is(f, "wp:inline")) return "inline";
    if (f.child("wp:wrapSquare")) return "square";
    if (f.child("wp:wrapTight")) return "tight";
    if (f.child("wp:wrapThrough")) return "through";
    if (f.child("wp:wrapTopAndBottom")) return "top_bottom";
    return std::string(xml::attr(f, "behindDoc")) == "1" ? "behind" : "in_front";
}
}  // namespace

std::vector<PictureInfo> list_pictures(Document& d) {
    std::vector<PictureInfo> v;
    int i = 0;
    for (auto f : picture_frames(d)) {
        auto ext = f.child("wp:extent");
        auto dp = f.child("wp:docPr");
        auto p = xml::ancestor(f, "w:p");
        v.push_back({++i, xml::attr(dp, "id"), xml::attr(dp, "name"), xml::attr(dp, "descr"), para_id(p), wrap_of(f),
                     ext.attribute("cx").as_double() / 12700.0, ext.attribute("cy").as_double() / 12700.0});
    }
    return v;
}

void set_picture(Document& d, int n, double wpt, double hpt, const Json& placement, const Json& alt) {
    auto f = frame_at(d, n);
    if (wpt > 0 || hpt > 0) {
        auto ext = f.child("wp:extent");
        double cx = ext.attribute("cx").as_double(), cy = ext.attribute("cy").as_double();
        double ncx = wpt > 0 ? wpt * 12700 : cx * (hpt * 12700 / std::max(1.0, cy));
        double ncy = hpt > 0 ? hpt * 12700 : cy * (wpt * 12700 / std::max(1.0, cx));
        if (ncx < 1 || ncy < 1 || ncx > 22 * 914400 || ncy > 22 * 914400)
            throw std::runtime_error("picture size must be up to 22 inches");
        auto put = [&](pugi::xml_node e) {
            e.attribute("cx").set_value(std::to_string(std::lround(ncx)).c_str());
            e.attribute("cy").set_value(std::to_string(std::lround(ncy)).c_str());
        };
        put(ext);
        if (auto xe = xml::first_descendant(f, "a:ext")) put(xe);
    }
    if (alt.is_string()) {
        auto s = util::xml_safe(alt.get<std::string>());
        xml::set_attr(f.child("wp:docPr"), "descr", s);
        if (auto c = xml::first_descendant(f, "pic:cNvPr")) xml::set_attr(c, "descr", s);
    }
    if (!placement.is_null()) {
        auto pl = placement_from_json(placement);
        if (pl.wrap == "inline") {
            if (xml::name_is(f, "wp:anchor")) {
                // back into the line: the frame keeps extent, docPr, graphic
                auto drw = f.parent();
                auto inl = drw.insert_child_before("wp:inline", f);
                for (auto a : {"distT", "distB", "distL", "distR"}) inl.append_attribute(a) = "0";
                for (auto a : {"wp14:anchorId", "wp14:editId"})
                    if (auto v = f.attribute(a)) inl.append_attribute(a) = v.value();
                for (auto q : {"wp:extent", "wp:effectExtent", "wp:docPr", "wp:cNvGraphicFramePr", "a:graphic"})
                    if (auto c = f.child(q)) inl.append_move(c);
                drw.remove_child(f);
            }
        } else if (xml::name_is(f, "wp:inline")) {
            make_floating(d, f, pl);
        } else {
            set_position(f, pl);
            set_wrap(f, pl);
        }
    }
}

void delete_picture(Document& d, int n, const EditOpts& o) {
    auto f = frame_at(d, n);
    auto run = xml::ancestor(f, "w:r");
    auto p = xml::ancestor(run, "w:p");
    // its visible offset: the picture is one object atom
    ParaText pt = analyze(p);
    for (std::size_t i = 0; i < pt.vis.size(); i++)
        if (pt.atoms[pt.vis[i]].run == run) {
            delete_text(d, p, (int)i, (int)i + 1, o);
            return;
        }
    // floating pictures aren't in the visible text: remove the run itself
    if (o.track ? *o.track : d.track_changes()) {
        auto del = run.parent().insert_child_before("w:del", run);
        stamp(d, del);
        del.append_move(run);
    } else {
        run.parent().remove_child(run);
    }
}

// ── breaks ───────────────────────────────────────────────────────────────

void insert_break(Document& d, pugi::xml_node p, int pos, const std::string& type, const EditOpts& o) {
    pugi::xml_document tmp;
    auto r = tmp.append_child("w:r");
    auto br = r.append_child("w:br");
    if (type == "page" || type == "column") xml::set_attr(br, "w:type", type);
    else if (type != "line" && type != "textWrapping") throw std::runtime_error("break type must be page, column or line");
    place_run(d, p, pos, r, o);
}

std::vector<pugi::xml_node> sections(Document& d) {
    std::vector<pugi::xml_node> v;
    for (auto b : blocks(d.body()))
        if (auto s = b.child("w:pPr").child("w:sectPr")) v.push_back(s);
    if (auto s = d.body().child("w:sectPr")) v.push_back(s);
    return v;
}

void insert_section_break(Document& d, pugi::xml_node p, const std::string& type, const EditOpts& o) {
    // The new sectPr on p ends the section and copies the following section's
    // page setup (Word copies the current section's properties).
    pugi::xml_node following;
    for (auto n = p; n; n = n.next_sibling()) {
        if (auto s = n.child("w:pPr").child("w:sectPr")) { following = s; break; }
    }
    if (!following) following = d.body().child("w:sectPr");
    auto ppr = schema::ensure_child(p, "w:pPr");
    if (ppr.child("w:sectPr")) throw std::runtime_error("that paragraph already ends a section");
    auto sp = ppr.append_copy(following);
    schema::place_child(ppr, sp);
    // header/footer references belong to the following section only when it
    // has them; the new one inherits by omission (Word links to previous).
    for (auto q : {"w:headerReference", "w:footerReference"})
        for (auto e : xml::children(sp, q)) sp.remove_child(e);
    sp.remove_child("w:sectPrChange");
    // the break type belongs to the section that STARTS after the break
    auto ty = schema::ensure_child(following, "w:type");
    static const std::set<std::string> ok = {"nextPage", "continuous", "evenPage", "oddPage", "nextColumn"};
    if (!ok.count(type)) throw std::runtime_error("section break type must be nextPage, continuous, evenPage, oddPage or nextColumn");
    if (type == "nextPage") following.remove_child(ty);
    else xml::set_attr(ty, "w:val", type);
    if (tracking(d, o)) {
        // Word tracks an inserted section break as an inserted paragraph mark
        auto rpr = schema::ensure_child(ppr, "w:rPr");
        if (!rpr.child("w:ins")) stamp(d, schema::ensure_child(rpr, "w:ins"));
    }
}

Json section_json(Document& d, pugi::xml_node s) {
    Json j;
    auto sz = s.child("w:pgSz");
    double w = util::to_double(xml::attr(sz, "w:w", "12240")) / 20, h = util::to_double(xml::attr(sz, "w:h", "15840")) / 20;
    j["page_width"] = w;
    j["page_height"] = h;
    j["orientation"] = xml::attr(sz, "w:orient", w > h ? "landscape" : "portrait");
    auto m = s.child("w:pgMar");
    Json mj;
    for (auto a : {"top", "bottom", "left", "right", "header", "footer", "gutter"})
        mj[a] = util::to_double(xml::attr(m, std::string("w:") + a, "0")) / 20;
    j["margins"] = mj;
    j["type"] = xml::attr(s.child("w:type"), "w:val", "nextPage");
    if (auto c = s.child("w:cols")) {
        j["columns"] = util::to_int(xml::attr(c, "w:num", "1"));
        j["column_space"] = util::to_double(xml::attr(c, "w:space", "720")) / 20;
        if (xml::attr(c, "w:sep") == "1" || xml::attr(c, "w:sep") == "true") j["column_line"] = true;
        if (c.child("w:col")) {
            Json cw = Json::array();
            for (auto col : xml::children(c, "w:col")) cw.push_back(util::to_double(xml::attr(col, "w:w", "0")) / 20);
            j["column_widths"] = cw;
        }
    }
    if (auto pb = s.child("w:pgBorders")) {
        Json bj = Json::object();
        for (auto side : {"top", "left", "bottom", "right"})
            if (auto b = pb.child((std::string("w:") + side).c_str()))
                bj[side] = {{"style", xml::attr(b, "w:val")}, {"size", util::to_double(xml::attr(b, "w:sz", "4")) / 8},
                            {"color", xml::attr(b, "w:color", "auto")}};
        bj["offset_from"] = xml::attr(pb, "w:offsetFrom", "text");
        j["page_borders"] = bj;
    }
    if (auto bg = d.body().parent().child("w:background")) j["page_color"] = xml::attr(bg, "w:color");
    j["title_page"] = (bool)s.child("w:titlePg");
    Json hf = Json::object();
    for (auto q : {"w:headerReference", "w:footerReference"}) {
        for (auto e : xml::children(s, q)) {
            std::string kind = std::string(q) == "w:headerReference" ? "header" : "footer";
            std::string which = xml::attr(e, "w:type", "default");
            auto rel = d.pkg().rels(d.main()).by_id(xml::attr(e, "r:id"));
            if (rel) hf[kind + "_" + which] = opc::Package::resolve(d.main(), rel->target);
        }
    }
    j["headers_footers"] = hf;
    if (auto pn = s.child("w:pgNumType")) {
        Json pj;
        if (pn.attribute("w:start")) pj["start"] = util::to_int(xml::attr(pn, "w:start"));
        if (pn.attribute("w:fmt")) pj["format"] = xml::attr(pn, "w:fmt");
        j["page_numbers"] = pj;
    }
    return j;
}

void format_section(Document& d, int index, const Json& props, const EditOpts& o) {
    auto ss = sections(d);
    if (index < 1 || index > (int)ss.size()) throw std::runtime_error("section " + std::to_string(index) + " not found");
    auto s = ss[index - 1];
    if (tracking(d, o) && !s.child("w:sectPrChange")) {
        pugi::xml_document old;
        auto op = old.append_child("w:sectPr");
        for (auto c : s.children()) {
            std::string_view q = c.name();
            if (q == "w:sectPrChange" || q == "w:headerReference" || q == "w:footerReference") continue;
            op.append_copy(c);
        }
        auto ch = schema::ensure_child(s, "w:sectPrChange");
        stamp(d, ch);
        ch.append_copy(op);
    }
    for (auto& [k, v] : props.items()) {
        if (k == "orientation") {
            auto sz = schema::ensure_child(s, "w:pgSz");
            long w = util::to_long(xml::attr(sz, "w:w", "12240")), h = util::to_long(xml::attr(sz, "w:h", "15840"));
            bool land = v.get<std::string>() == "landscape";
            if (land != (w > h)) { xml::set_attr(sz, "w:w", std::to_string(h)); xml::set_attr(sz, "w:h", std::to_string(w)); }
            if (land) xml::set_attr(sz, "w:orient", "landscape");
            else xml::remove_attr(sz, "w:orient");
        } else if (k == "page_width") xml::set_attr(schema::ensure_child(s, "w:pgSz"), "w:w", std::to_string(twips_of(v)));
        else if (k == "page_height") xml::set_attr(schema::ensure_child(s, "w:pgSz"), "w:h", std::to_string(twips_of(v)));
        else if (k == "paper") {
            static const std::map<std::string, std::pair<long, long>> sizes = {
                {"letter", {12240, 15840}}, {"legal", {12240, 20160}}, {"a4", {11906, 16838}}, {"a3", {16838, 23811}},
                {"a5", {8391, 11906}}, {"b5", {10319, 14571}}, {"executive", {10440, 15120}}, {"tabloid", {15840, 24480}}};
            auto it = sizes.find(util::lower(v.get<std::string>()));
            if (it == sizes.end()) throw std::runtime_error("unknown paper size");
            auto sz = schema::ensure_child(s, "w:pgSz");
            bool land = xml::attr(sz, "w:orient") == "landscape";
            xml::set_attr(sz, "w:w", std::to_string(land ? it->second.second : it->second.first));
            xml::set_attr(sz, "w:h", std::to_string(land ? it->second.first : it->second.second));
        } else if (k == "margins") {
            auto m = schema::ensure_child(s, "w:pgMar");
            for (auto a : {"top", "right", "bottom", "left", "header", "footer", "gutter"})
                if (!m.attribute((std::string("w:") + a).c_str())) xml::set_attr(m, std::string("w:") + a, a == std::string("header") || a == std::string("footer") ? "720" : a == std::string("gutter") ? "0" : "1440");
            for (auto& [mk, mv] : v.items()) xml::set_attr(m, "w:" + mk, std::to_string(twips_of(mv)));
        } else if (k == "columns") {
            auto c = schema::ensure_child(s, "w:cols");
            xml::set_attr(c, "w:num", std::to_string(v.get<int>()));
            if (!c.attribute("w:space")) xml::set_attr(c, "w:space", "720");
        } else if (k == "column_space") xml::set_attr(schema::ensure_child(s, "w:cols"), "w:space", std::to_string(twips_of(v)));
        else if (k == "type") {
            if (v.get<std::string>() == "nextPage") s.remove_child("w:type");
            else enum_attr(schema::ensure_child(s, "w:type"), "w:val", v.get<std::string>());
        } else if (k == "title_page") { if (v.get<bool>()) schema::ensure_child(s, "w:titlePg"); else s.remove_child("w:titlePg"); }
        else if (k == "vertical_align") enum_attr(schema::ensure_child(s, "w:vAlign"), "w:val", v.get<std::string>());
        else if (k == "line_numbers") {
            if (v.is_null() || (v.is_boolean() && !v.get<bool>())) { s.remove_child("w:lnNumType"); continue; }
            auto l = schema::ensure_child(s, "w:lnNumType");
            xml::set_attr(l, "w:countBy", std::to_string(v.is_object() ? v.value("count_by", 1) : 1));
            xml::set_attr(l, "w:restart", v.is_object() ? v.value("restart", std::string("newPage")) : "newPage");
        } else if (k == "page_numbers") {
            auto pn = schema::ensure_child(s, "w:pgNumType");
            if (v.contains("start")) xml::set_attr(pn, "w:start", std::to_string(v["start"].get<int>()));
            if (v.contains("format")) xml::set_attr(pn, "w:fmt", v["format"].get<std::string>());
        } else if (k == "column_line") {
            auto c = schema::ensure_child(s, "w:cols");
            if (v.get<bool>()) xml::set_attr(c, "w:sep", "1");
            else xml::remove_attr(c, "w:sep");
        } else if (k == "column_widths") {
            // unequal columns: [width, ...] or [{width, space}, ...]
            auto c = schema::ensure_child(s, "w:cols");
            while (c.child("w:col")) c.remove_child("w:col");
            if (v.is_null() || v.empty()) { xml::remove_attr(c, "w:equalWidth"); continue; }
            xml::set_attr(c, "w:num", std::to_string(v.size()));
            xml::set_attr(c, "w:equalWidth", "0");
            std::string gap = xml::attr(c, "w:space", "720");
            for (std::size_t i = 0; i < v.size(); i++) {
                auto col = c.append_child("w:col");
                const Json& w = v[i].is_object() ? v[i].value("width", Json(nullptr)) : v[i];
                if (w.is_null()) throw std::runtime_error("column_widths: each entry needs a width");
                xml::set_attr(col, "w:w", std::to_string(twips_of(w)));
                if (i + 1 < v.size())
                    xml::set_attr(col, "w:space", v[i].is_object() && v[i].contains("space") ? std::to_string(twips_of(v[i]["space"])) : gap);
            }
        } else if (k == "page_borders") {
            // "single" | {style,size,color,space} for all sides | {top:..., left:..., offset_from:"page"|"text"}
            if (v.is_null() || (v.is_boolean() && !v.get<bool>())) { s.remove_child("w:pgBorders"); continue; }
            auto pb = schema::ensure_child(s, "w:pgBorders");
            static const char* sides[] = {"top", "left", "bottom", "right"};
            bool per_side = v.is_object() && (v.contains("top") || v.contains("left") || v.contains("bottom") || v.contains("right"));
            std::string from = v.is_object() ? v.value("offset_from", std::string("page")) : "page";
            if (from != "page" && from != "text") throw std::runtime_error("page_borders.offset_from must be page or text");
            if (from == "page") xml::set_attr(pb, "w:offsetFrom", "page");
            else xml::remove_attr(pb, "w:offsetFrom");
            Json all = v;
            if (all.is_object()) all.erase("offset_from");
            for (auto side : sides) {
                Json bv = per_side ? (v.contains(side) ? v[side] : Json(nullptr)) : all;
                std::string q = std::string("w:") + side;
                if (bv.is_null()) { if (per_side && v.contains(side)) pb.remove_child(q.c_str()); continue; }
                // Word measures page-border space from the page edge in points (24 is its default)
                if (from == "page" && (!bv.is_object() || !bv.contains("space"))) {
                    if (!bv.is_object()) bv = Json{{"style", bv.is_string() ? bv.get<std::string>() : "single"}};
                    bv["space"] = 24;
                }
                set_border(schema::ensure_child(pb, q), bv);
            }
        } else if (k == "page_color") {
            // document-wide: w:background, shown once settings asks for it
            auto body_root = d.body().parent();
            if (v.is_null()) {
                body_root.remove_child("w:background");
                d.settings().remove_child("w:displayBackgroundShape");
                continue;
            }
            auto bg = body_root.child("w:background");
            if (!bg) bg = body_root.prepend_child("w:background");
            xml::set_attr(bg, "w:color", color_hex(v));
            schema::ensure_child(d.settings(), "w:displayBackgroundShape");
        } else throw std::runtime_error(util::unknown_key("page setup key", k, {"orientation", "paper", "page_width", "page_height", "margins", "columns", "column_space", "column_line", "column_widths", "type", "title_page", "vertical_align", "line_numbers", "page_numbers", "page_borders", "page_color"}));
    }
    // pPr even-and-odd headers live in settings
    if (props.contains("even_odd_headers")) {
        auto st = d.settings();
        if (props["even_odd_headers"].get<bool>()) schema::ensure_child(st, "w:evenAndOddHeaders");
        else st.remove_child("w:evenAndOddHeaders");
    }
}

// ── headers / footers ────────────────────────────────────────────────────

std::string header_footer(Document& d, int section, const std::string& kind, const std::string& which, bool create) {
    auto ss = sections(d);
    if (section < 1 || section > (int)ss.size()) throw std::runtime_error("section not found");
    if (kind != "header" && kind != "footer") throw std::runtime_error("kind must be header or footer");
    if (which != "default" && which != "first" && which != "even") throw std::runtime_error("which must be default, first or even");
    auto s = ss[section - 1];
    std::string q = "w:" + kind + "Reference";
    for (auto e : xml::children(s, q))
        if (xml::attr(e, "w:type", "default") == which) {
            auto rel = d.pkg().rels(d.main()).by_id(xml::attr(e, "r:id"));
            if (rel) return opc::Package::resolve(d.main(), rel->target);
        }
    if (!create) return "";
    std::string root = "w:hdr";
    if (kind == "footer") root = "w:ftr";
    ensure_style(d, kind == "header" ? "Header" : "Footer");
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n" + wml_root_open(root.c_str()) +
                    "<w:p w14:paraId=\"" + d.new_hex_id() + "\" w14:textId=\"77777777\"><w:pPr><w:pStyle w:val=\"" +
                    (kind == "header" ? "Header" : "Footer") + "\"/></w:pPr></w:p></" + root + ">";
    std::string name = d.pkg().unique_name("/word/" + kind, ".xml");
    d.pkg().add_xml_part(name, kind == "header" ? opc::ct::header : opc::ct::footer, x);
    std::string rid = d.pkg().rels(d.main()).add(kind == "header" ? opc::rt::header : opc::rt::footer, opc::Package::relative(d.main(), name));
    // references come first in sectPr, headers before footers (schema order is a choice group)
    auto ref = s.prepend_child(q.c_str());
    xml::set_attr(ref, "w:type", which);
    xml::set_attr(ref, "r:id", rid);
    if (which == "first") schema::ensure_child(s, "w:titlePg");
    if (which == "even") schema::ensure_child(d.settings(), "w:evenAndOddHeaders");
    xml::declare(xml::root_of(s), "r");
    return name;
}

void set_header_footer_text(Document& d, int section, const std::string& kind, const std::string& which,
                            const std::string& text, const Json& opts_in) {
    const Json opts = opts_in.is_object() ? opts_in : Json::object();
    std::string part = header_footer(d, section, kind, which, true);
    auto root = d.pkg().xml(part).document_element();
    // replace content with paragraphs; supports {PAGE} {NUMPAGES} {DATE} placeholders as fields
    std::string style = kind == "header" ? "Header" : "Footer";
    std::vector<pugi::xml_node> old;
    for (auto c : root.children()) old.push_back(c);
    for (auto c : old) root.remove_child(c);
    for (auto& line : util::split(text, '\n')) {
        auto p = root.append_child("w:p");
        p.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        p.append_attribute("w14:textId") = "77777777";
        auto ppr = p.append_child("w:pPr");
        xml::set_attr(ppr.append_child("w:pStyle"), "w:val", style);
        if (opts.contains("alignment")) {
            std::string a = opts["alignment"].get<std::string>();
            if (a == "justify") a = "both";
            xml::set_attr(schema::ensure_child(ppr, "w:jc"), "w:val", a);
        }
        // split on {FIELD} placeholders
        std::size_t i = 0;
        while (i < line.size()) {
            auto b = line.find('{', i);
            auto e = b == std::string::npos ? std::string::npos : line.find('}', b);
            std::string lit = line.substr(i, (b == std::string::npos || e == std::string::npos ? line.size() : b) - i);
            if (!lit.empty()) append_text(p.append_child("w:r"), util::to_u32(lit));
            if (b == std::string::npos || e == std::string::npos) break;
            std::string instr = line.substr(b + 1, e - b - 1);
            std::string up = util::lower(instr);
            std::string cached = up == "page" ? "1" : up == "numpages" || up == "sectionpages" ? "1" : "";
            int pos = (int)analyze(p).vis.size();
            insert_field(d, p, pos, util::trim(instr).find(' ') == std::string::npos ? std::string(util::trim(instr)) : instr, cached, nullptr, {false});
            i = e + 1;
        }
    }
}

// ── watermark ──────────────────────────────────────────────────────────────
// Word's Design > Watermark: a VML text path in each header, behind the text.

namespace {
bool is_watermark_run(pugi::xml_node r) {
    for (auto pict : xml::children(r, "w:pict"))
        for (auto sh : pict.children())
            if (std::string(xml::attr(sh, "id")).rfind("PowerPlusWaterMarkObject", 0) == 0) return true;
    return false;
}

std::vector<std::string> header_parts_for_watermark(Document& d, bool create) {
    std::vector<std::string> parts;
    auto ss = sections(d);
    for (int i = 1; i <= (int)ss.size(); i++)
        for (auto which : {"default", "first", "even"}) {
            std::string part = header_footer(d, i, "header", which, create && i == 1 && std::string(which) == "default");
            if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
        }
    return parts;
}

int remove_watermarks_in(pugi::xml_node root) {
    std::vector<pugi::xml_node> runs;
    for (auto r : root.select_nodes(".//w:r")) if (is_watermark_run(r.node())) runs.push_back(r.node());
    for (auto r : runs) r.parent().remove_child(r);
    return (int)runs.size();
}
}  // namespace

int remove_watermark(Document& d) {
    int n = 0;
    for (auto& part : header_parts_for_watermark(d, false)) n += remove_watermarks_in(d.pkg().xml(part).document_element());
    return n;
}

int set_watermark(Document& d, const std::string& text, const Json& opts_in) {
    if (util::trim(text).empty()) throw std::runtime_error("watermark text is empty (use remove to take it off)");
    const Json o = opts_in.is_object() ? opts_in : Json::object();
    std::string font = o.value("font", std::string("Calibri"));
    std::string color = o.contains("color") ? color_hex(o["color"]) : "C0C0C0";
    bool diagonal = o.value("layout", std::string("diagonal")) != "horizontal";
    double opacity = o.value("opacity", 0.5);
    if (opacity <= 0 || opacity > 1) throw std::runtime_error("opacity must be between 0 and 1");
    // Word sizes the shape to the text: ~0.6 em per character at 1pt font
    double size = o.value("size", 0.0);
    auto u = util::to_u32(text);
    double width = size > 0 ? size * 0.6 * u.size() : 468.0;
    if (size <= 0) size = std::max(20.0, std::min(200.0, width / (0.6 * std::max<std::size_t>(1, u.size()))));
    double height = size * 1.2;
    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '&') r += "&amp;"; else if (c == '<') r += "&lt;"; else if (c == '>') r += "&gt;"; else if (c == '"') r += "&quot;";
            else r += c;
        }
        return r;
    };
    char buf[64];
    auto num = [&](double v) { std::snprintf(buf, sizeof buf, "%.1f", v); return std::string(buf); };
    int n = 0;
    for (auto& part : header_parts_for_watermark(d, true)) {
        auto root = d.pkg().xml(part).document_element();
        remove_watermarks_in(root);
        xml::declare(root, "v");
        xml::declare(root, "o");
        xml::declare(root, "w10");
        std::string id = "PowerPlusWaterMarkObject" + std::to_string(d.next_annotation_id());
        std::string frag =
            "<w:r><w:rPr><w:noProof/></w:rPr><w:pict>"
            "<v:shapetype id=\"_x0000_t136\" coordsize=\"21600,21600\" o:spt=\"136\" adj=\"10800\" path=\"m@7,l@8,m@5,21600l@6,21600e\">"
            "<v:formulas><v:f eqn=\"sum #0 0 10800\"/><v:f eqn=\"prod #0 2 1\"/><v:f eqn=\"sum 21600 0 @1\"/><v:f eqn=\"sum 0 0 @2\"/>"
            "<v:f eqn=\"sum 21600 0 @3\"/><v:f eqn=\"if @0 @3 0\"/><v:f eqn=\"if @0 21600 @1\"/><v:f eqn=\"if @0 0 @2\"/>"
            "<v:f eqn=\"if @0 @4 21600\"/><v:f eqn=\"mid @5 @6\"/><v:f eqn=\"mid @8 @5\"/><v:f eqn=\"mid @7 @8\"/>"
            "<v:f eqn=\"mid @6 @7\"/><v:f eqn=\"sum @6 0 @5\"/></v:formulas>"
            "<v:path textpathok=\"t\" o:connecttype=\"custom\" o:connectlocs=\"@9,0;@10,10800;@11,21600;@12,10800\" o:connectangles=\"270,180,90,0\"/>"
            "<v:textpath on=\"t\" fitshape=\"t\"/><v:handles><v:h position=\"#0,bottomRight\" xrange=\"6629,14971\"/></v:handles>"
            "<o:lock v:ext=\"edit\" text=\"t\" shapetype=\"t\"/></v:shapetype>"
            "<v:shape id=\"" + id + "\" o:spid=\"_x0000_s" + std::to_string(2049 + n) + "\" type=\"#_x0000_t136\" "
            "style=\"position:absolute;margin-left:0;margin-top:0;width:" + num(width) + "pt;height:" + num(height) + "pt;" +
            (diagonal ? "rotation:315;" : "") +
            "z-index:-251657216;mso-position-horizontal:center;mso-position-horizontal-relative:margin;"
            "mso-position-vertical:center;mso-position-vertical-relative:margin\" o:allowincell=\"f\" fillcolor=\"#" + color +
            "\" stroked=\"f\"><v:fill opacity=\"" + num(opacity) + "\"/>"
            "<v:textpath style=\"font-family:&quot;" + esc(font) + "&quot;;font-size:1pt\" string=\"" + esc(text) + "\"/>"
            "<w10:wrap anchorx=\"margin\" anchory=\"margin\"/></v:shape></w:pict></w:r>";
        pugi::xml_document holder;
        std::string err;
        if (!xml::parse_fragment(frag, holder, &err)) throw std::runtime_error("watermark: " + err);
        auto p = root.child("w:p");
        if (!p) {
            p = root.append_child("w:p");
            xml::set_attr(schema::ensure_child(p, "w:pPr").append_child("w:pStyle"), "w:val", "Header");
        }
        // first in the paragraph, after pPr
        pugi::xml_node before = p.child("w:pPr") ? p.child("w:pPr").next_sibling() : p.first_child();
        for (auto c : holder.children()) xml::copy_into(p, c, before);
        n++;
    }
    return n;
}

void remove_header_footer(Document& d, int section, const std::string& kind, const std::string& which) {
    auto ss = sections(d);
    if (section < 1 || section > (int)ss.size()) throw std::runtime_error("section not found");
    auto s = ss[section - 1];
    std::string q = "w:" + kind + "Reference";
    for (auto e : xml::children(s, q)) {
        if (xml::attr(e, "w:type", "default") != which) continue;
        std::string rid = xml::attr(e, "r:id");
        s.remove_child(e);
        // remove the part if nothing else references it
        bool used = false;
        for (auto o : sections(d))
            for (auto r2 : xml::children(o, q)) if (xml::attr(r2, "r:id") == rid) used = true;
        if (!used) {
            auto& rels = d.pkg().rels(d.main());
            if (auto rel = rels.by_id(rid)) {
                std::string name = opc::Package::resolve(d.main(), rel->target);
                rels.remove(rid);
                d.pkg().remove_part(name);
            }
        }
    }
}

// ── notes ────────────────────────────────────────────────────────────────

std::string add_note(Document& d, pugi::xml_node p, int pos, const std::string& kind, const std::string& text, const EditOpts& o) {
    if (kind != "footnote" && kind != "endnote") throw std::runtime_error("kind must be footnote or endnote");
    bool fn = kind == "footnote";
    auto root = fn ? d.footnotes() : d.endnotes();
    int id = 1;
    for (auto n : root.children(fn ? "w:footnote" : "w:endnote")) id = std::max(id, n.attribute("w:id").as_int() + 1);
    std::string sty_text = ensure_style(d, fn ? "FootnoteText" : "EndnoteText");
    std::string sty_ref = ensure_style(d, fn ? "FootnoteReference" : "EndnoteReference");
    auto note = root.append_child(fn ? "w:footnote" : "w:endnote");
    xml::set_attr(note, "w:id", std::to_string(id));
    bool first = true;
    for (auto& line : util::split(text, '\n')) {
        auto np = note.append_child("w:p");
        np.append_attribute("w14:paraId") = d.new_hex_id().c_str();
        np.append_attribute("w14:textId") = "77777777";
        xml::set_attr(np.append_child("w:pPr").append_child("w:pStyle"), "w:val", sty_text);
        if (first) {
            auto r = np.append_child("w:r");
            xml::set_attr(r.append_child("w:rPr").append_child("w:rStyle"), "w:val", sty_ref);
            r.append_child(fn ? "w:footnoteRef" : "w:endnoteRef");
            append_text(np.append_child("w:r"), U" ");
            first = false;
        }
        if (!line.empty()) append_text(np.append_child("w:r"), util::to_u32(line));
    }
    xml::declare(root, "w14", true);
    pugi::xml_document tmp;
    auto r = tmp.append_child("w:r");
    xml::set_attr(r.append_child("w:rPr").append_child("w:rStyle"), "w:val", sty_ref);
    xml::set_attr(r.append_child(fn ? "w:footnoteReference" : "w:endnoteReference"), "w:id", std::to_string(id));
    place_run(d, p, pos, r, o);
    if (tracking(d, o)) {
        // the note's own text is part of the insertion
        for (auto np : note.children("w:p")) {
            std::vector<pugi::xml_node> runs;
            for (auto rr : np.children("w:r")) runs.push_back(rr);
            for (auto rr : runs) {
                auto ins = np.insert_child_before("w:ins", rr);
                stamp(d, ins);
                ins.append_move(rr);
            }
        }
    }
    return std::to_string(id);
}

std::vector<NoteInfo> list_notes(Document& d) {
    std::vector<NoteInfo> v;
    std::map<std::string, std::string> anchor;
    for (auto p : paragraphs(d.body())) {
        for (auto q : {"w:footnoteReference", "w:endnoteReference"})
            for (auto r : xml::descendants(p, q))
                anchor[std::string(q).substr(2, 3) + xml::attr(r, "w:id")] = xml::attr(p, "w14:paraId");
    }
    for (auto [kind, root] : {std::pair{std::string("footnote"), d.footnotes(false)}, std::pair{std::string("endnote"), d.endnotes(false)}}) {
        if (!root) continue;
        for (auto n : root.children(("w:" + kind).c_str())) {
            std::string t = xml::attr(n, "w:type");
            if (t == "separator" || t == "continuationSeparator" || t == "continuationNotice") continue;
            NoteInfo i;
            i.id = xml::attr(n, "w:id");
            i.kind = kind;
            for (auto p : n.children("w:p")) i.text += (i.text.empty() ? "" : "\n") + util::trim(analyze(p).utf8());
            // strip the reference mark placeholder
            if (!i.text.empty() && i.text[0] == ' ') i.text = i.text.substr(1);
            i.anchor_para = anchor[kind.substr(0, 3) + i.id];
            v.push_back(i);
        }
    }
    return v;
}

void delete_note(Document& d, const std::string& kind, const std::string& id, const EditOpts& o) {
    bool fn = kind == "footnote";
    std::string q = fn ? "w:footnoteReference" : "w:endnoteReference";
    for (auto r : xml::descendants(d.body(), q)) {
        if (xml::attr(r, "w:id") != id) continue;
        auto run = r.parent();
        if (tracking(d, o)) {
            auto del = run.parent().insert_child_before("w:del", run);
            stamp(d, del);
            del.append_move(run);
            return;  // Word keeps the note until the deletion is accepted
        }
        run.parent().remove_child(run);
    }
    auto root = fn ? d.footnotes(false) : d.endnotes(false);
    if (root)
        for (auto n : xml::children(root, fn ? "w:footnote" : "w:endnote"))
            if (xml::attr(n, "w:id") == id) root.remove_child(n);
}

// ── bookmarks ────────────────────────────────────────────────────────────

void add_bookmark(Document& d, pugi::xml_node p1, int s, pugi::xml_node p2, int e, const std::string& name) {
    if (name.empty() || name.size() > 40) throw std::runtime_error("bookmark names are 1-40 characters");
    if (!(std::isalpha((unsigned char)name[0]) || name[0] == '_')) throw std::runtime_error("bookmark names start with a letter");
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_')) throw std::runtime_error("bookmark names use letters, digits and _ only");
    for (auto& b : list_bookmarks(d))
        if (util::iequals(b.name, name)) throw std::runtime_error("bookmark '" + name + "' already exists");
    std::string id = std::to_string(d.next_bookmark_id());
    auto put = [&](pugi::xml_node p, int pos, bool start) {
        split_at(p, pos);
        ParaText pt = analyze(p);
        int n = (int)pt.vis.size();
        auto top = [&](pugi::xml_node x) { while (std::string_view(x.parent().name()) != "w:p") x = x.parent(); return x; };
        pugi::xml_node b;
        const char* q = start ? "w:bookmarkStart" : "w:bookmarkEnd";
        if (start ? pos < n : (pos < n && pos > 0)) {
            if (start) b = p.insert_child_before(q, top(pt.atoms[pt.vis[pos]].run));
            else {
                // ending on a field result: close after the field's end mark
                auto last = top(pt.atoms[pt.vis[pos - 1]].run);
                auto nx = past_field_result(last);
                b = nx ? p.insert_child_before(q, nx) : p.append_child(q);
            }
        }
        else if (!start && pos > 0 && n > 0) {
            auto last = top(pt.atoms[pt.vis[std::min(pos, n) - 1]].run);
            auto nx = past_field_result(last);
            b = nx ? p.insert_child_before(q, nx) : p.append_child(q);
        }
        else if (start && n > 0) b = p.insert_child_after(q, top(pt.atoms[pt.vis[n - 1]].run));
        else {
            auto ppr = p.child("w:pPr");
            b = ppr ? p.insert_child_after(q, ppr) : p.prepend_child(q);
        }
        return b;
    };
    auto bs = put(p1, s, true);
    xml::set_attr(bs, "w:id", id);
    xml::set_attr(bs, "w:name", name);
    pugi::xml_node be;
    if (p1 == p2 && s == e) be = bs.parent().insert_child_after("w:bookmarkEnd", bs);
    else be = put(p2, e, false);
    xml::set_attr(be, "w:id", id);
}

void delete_bookmark(Document& d, const std::string& name) {
    bool found = false;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        std::string id;
        for (auto b : xml::descendants(root, "w:bookmarkStart"))
            if (xml::attr(b, "w:name") == name) { id = xml::attr(b, "w:id"); b.parent().remove_child(b); found = true; }
        if (!id.empty())
            for (auto b : xml::descendants(root, "w:bookmarkEnd"))
                if (xml::attr(b, "w:id") == id) b.parent().remove_child(b);
    }
    if (!found) throw std::runtime_error("bookmark '" + name + "' not found");
}

std::vector<BookmarkInfo> list_bookmarks(Document& d) {
    std::vector<BookmarkInfo> v;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        std::map<std::string, std::size_t> open;
        std::function<void(pugi::xml_node, pugi::xml_node)> walk = [&](pugi::xml_node n, pugi::xml_node para) {
            for (auto k : n.children()) {
                if (k.type() != pugi::node_element) continue;
                std::string_view q = k.name();
                if (q == "w:bookmarkStart") {
                    BookmarkInfo b;
                    b.name = xml::attr(k, "w:name");
                    b.id = xml::attr(k, "w:id");
                    b.para = xml::attr(para, "w14:paraId");
                    open[b.id] = v.size();
                    v.push_back(b);
                } else if (q == "w:bookmarkEnd") {
                    open.erase(xml::attr(k, "w:id"));
                } else if (q == "w:t") {
                    for (auto& [id, idx] : open) v[idx].text += k.text().get();
                } else if (q == "w:p") {
                    walk(k, k);
                    for (auto& [id, idx] : open) v[idx].text += "\n";
                } else if (q != "w:delText" && q != "w:instrText" && q != "mc:Fallback") {
                    walk(k, para);
                }
            }
        };
        walk(root, {});
    }
    for (auto& b : v) while (!b.text.empty() && b.text.back() == '\n') b.text.pop_back();
    return v;
}

// ── hyperlinks ───────────────────────────────────────────────────────────

void add_hyperlink(Document& d, pugi::xml_node p, int s, int e, const std::string& target, const std::string& text,
                   const std::string& tooltip, const EditOpts& o) {
    if (target.empty()) throw std::runtime_error("hyperlink target is empty");
    std::string style = ensure_style(d, "Hyperlink");
    if (!text.empty()) {
        // replace [s,e) (or insert at s) with the link text
        if (e > s) replace_text(d, p, s, e, util::to_u32(text), {{"style", style}}, o);
        else insert_text(d, p, s, util::to_u32(text), {{"style", style}}, o);
        e = s + (int)util::u32_length(text);
    } else {
        if (e <= s) throw std::runtime_error("give link text or a non-empty range");
        format_runs(d, p, s, e, {{"style", style}}, o);
    }
    auto runs = isolate(p, s, e);
    if (runs.empty()) return;
    // the hyperlink wraps the outermost nodes (keeping revision containers inside)
    auto top = [&](pugi::xml_node x) { while (std::string_view(x.parent().name()) != "w:p") x = x.parent(); return x; };
    pugi::xml_node first = top(runs.front()), last = top(runs.back());
    auto h = p.insert_child_before("w:hyperlink", first);
    if (target[0] == '#') xml::set_attr(h, "w:anchor", target.substr(1));
    else {
        std::string rid = d.pkg().rels(d.main()).add(opc::rt::hyperlink, target, opc::TargetMode::External);
        xml::set_attr(h, "r:id", rid);
        xml::declare(xml::root_of(p), "r");
    }
    if (!tooltip.empty()) xml::set_attr(h, "w:tooltip", tooltip);
    xml::set_attr(h, "w:history", "1");
    std::vector<pugi::xml_node> mv;
    for (auto n = first; n; n = n.next_sibling()) {
        mv.push_back(n);
        if (n == last) break;
    }
    for (auto n : mv) h.append_move(n);
}

std::vector<LinkInfo> list_hyperlinks(Document& d) {
    std::vector<LinkInfo> v;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto h : xml::descendants(root, "w:hyperlink")) {
            LinkInfo li;
            std::string rid = xml::attr(h, "r:id");
            if (!rid.empty()) {
                if (auto rel = d.pkg().rels(part).by_id(rid)) li.target = rel->target;
            }
            if (!xml::attr(h, "w:anchor").empty()) li.target += "#" + xml::attr(h, "w:anchor");
            for (auto t : xml::descendants(h, "w:t")) li.text += t.text().get();
            if (auto p = xml::ancestor(h, "w:p")) li.para = xml::attr(p, "w14:paraId");
            v.push_back(li);
        }
        // HYPERLINK fields too
        for (auto it : xml::descendants(root, "w:instrText")) {
            std::string s = util::trim(it.text().get());
            if (!util::starts_with(s, "HYPERLINK")) continue;
            LinkInfo li;
            auto q1 = s.find('"'), q2 = s.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) li.target = s.substr(q1 + 1, q2 - q1 - 1);
            if (auto p = xml::ancestor(it, "w:p")) li.para = xml::attr(p, "w14:paraId");
            v.push_back(li);
        }
    }
    return v;
}

int remove_hyperlinks(Document& d, const std::string& sub) {
    int n = 0;
    for (auto& part : d.story_parts()) {
        if (!d.pkg().has(part)) continue;
        auto root = d.pkg().xml(part).document_element();
        for (auto h : xml::descendants(root, "w:hyperlink")) {
            std::string tgt;
            if (auto rel = d.pkg().rels(part).by_id(xml::attr(h, "r:id"))) tgt = rel->target;
            tgt += xml::attr(h, "w:anchor");
            if (!sub.empty() && tgt.find(sub) == std::string::npos) continue;
            std::string rid = xml::attr(h, "r:id");
            auto parent = h.parent();
            for (auto c = h.first_child(); c;) {
                auto nx = c.next_sibling();
                parent.insert_move_before(c, h);
                c = nx;
            }
            parent.remove_child(h);
            if (!rid.empty()) {
                bool used = false;
                for (auto o : xml::descendants(root, "w:hyperlink")) if (xml::attr(o, "r:id") == rid) used = true;
                if (!used) d.pkg().rels(part).remove(rid);
            }
            n++;
        }
    }
    return n;
}

}  // namespace docx::wml
