#include "opc/package.hpp"

#include <map>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <miniz.h>

#include "util/fsio.hpp"

namespace docx::opc {

namespace {

constexpr const char* NS_CT = "http://schemas.openxmlformats.org/package/2006/content-types";
constexpr const char* NS_RELS = "http://schemas.openxmlformats.org/package/2006/relationships";

std::string pct_decode(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            o += char(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string ext_of(std::string_view name) {
    auto slash = name.rfind('/');
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return "";
    return util::lower(name.substr(dot + 1));
}

// "/word/_rels/document.xml.rels" -> "/word/document.xml"; "/_rels/.rels" -> "/"
std::optional<std::string> rels_source(std::string_view name) {
    auto slash = name.rfind('/');
    if (slash == std::string_view::npos) return std::nullopt;
    std::string_view dir = name.substr(0, slash);
    std::string_view file = name.substr(slash + 1);
    if (file.size() < 5 || util::lower(file.substr(file.size() - 5)) != ".rels") return std::nullopt;
    if (dir.size() < 6 || util::lower(dir.substr(dir.size() - 6)) != "/_rels") {
        if (util::lower(dir) != "/_rels") return std::nullopt;
    }
    std::string_view parent = dir.substr(0, dir.size() - 6);
    std::string src = std::string(parent) + "/" + std::string(file.substr(0, file.size() - 5));
    if (src == "/") return std::string("/");
    return src;
}

std::string rels_name_of(std::string_view source) {
    if (source == "/") return "/_rels/.rels";
    auto slash = source.rfind('/');
    return std::string(source.substr(0, slash)) + "/_rels/" + std::string(source.substr(slash + 1)) + ".rels";
}

Rels parse_rels(const util::Bytes& b) {
    Rels r;
    pugi::xml_document d;
    if (!d.load_buffer(b.data(), b.size())) throw std::runtime_error("malformed relationships part");
    for (auto e : d.document_element().children()) {
        std::string n = e.name();
        if (n != "Relationship" && !n.ends_with(":Relationship")) continue;
        Relationship rel;
        rel.id = e.attribute("Id").value();
        rel.type = e.attribute("Type").value();
        rel.target = e.attribute("Target").value();
        rel.mode = std::string(e.attribute("TargetMode").value()) == "External" ? TargetMode::External
                                                                                : TargetMode::Internal;
        r.items.push_back(std::move(rel));
    }
    return r;
}

struct StringWriter : pugi::xml_writer {
    std::string out;
    void write(const void* data, std::size_t size) override { out.append(static_cast<const char*>(data), size); }
};

const char* XML_DECL = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

std::string serialize_rels(const Rels& r) {
    pugi::xml_document d;
    auto root = d.append_child("Relationships");
    root.append_attribute("xmlns") = NS_RELS;
    for (auto& rel : r.items) {
        auto e = root.append_child("Relationship");
        e.append_attribute("Id") = rel.id.c_str();
        e.append_attribute("Type") = rel.type.c_str();
        e.append_attribute("Target") = rel.target.c_str();
        if (rel.mode == TargetMode::External) e.append_attribute("TargetMode") = "External";
    }
    return xml_to_string(d);
}

}  // namespace

std::string key_of(std::string_view name) { return util::lower(pct_decode(name)); }

std::string xml_to_string(const pugi::xml_document& d) {
    StringWriter w;
    // we write our own declaration; skip any parsed one
    for (auto c : d.children()) {
        if (c.type() == pugi::node_declaration) continue;
        c.print(w, "", pugi::format_raw, pugi::encoding_utf8);
    }
    return XML_DECL + w.out;
}

// ── Rels ─────────────────────────────────────────────────────────────────
const Relationship* Rels::by_id(std::string_view id) const {
    for (auto& r : items) if (r.id == id) return &r;
    return nullptr;
}
std::vector<const Relationship*> Rels::by_type(std::string_view type) const {
    std::vector<const Relationship*> v;
    for (auto& r : items) if (r.type == type) v.push_back(&r);
    return v;
}
const Relationship* Rels::first_of(std::string_view type) const {
    for (auto& r : items) if (r.type == type) return &r;
    return nullptr;
}
std::string Rels::next_id() const {
    for (int i = 1;; i++) {
        std::string id = "rId" + std::to_string(i);
        if (!by_id(id)) return id;
    }
}
std::string Rels::add(std::string type, std::string target, TargetMode mode) {
    Relationship r{next_id(), std::move(type), std::move(target), mode};
    items.push_back(r);
    return r.id;
}
bool Rels::remove(std::string_view id) {
    auto it = std::find_if(items.begin(), items.end(), [&](auto& r) { return r.id == id; });
    if (it == items.end()) return false;
    items.erase(it);
    return true;
}

// ── Package ──────────────────────────────────────────────────────────────
Package Package::open(const std::string& path) {
    return from_bytes(util::read_file(path));
}

Package Package::from_bytes(const util::Bytes& zip) {
    if (zip.size() >= 8 && zip[0] == 0xD0 && zip[1] == 0xCF && zip[2] == 0x11 && zip[3] == 0xE0)
        throw std::runtime_error("file is an OLE compound file (encrypted .docx or legacy .doc), not an OPC zip package");
    mz_zip_archive za{};
    if (!mz_zip_reader_init_mem(&za, zip.data(), zip.size(), 0))
        throw std::runtime_error("not a zip package (is this really a .docx?)");
    struct Guard {
        mz_zip_archive* z;
        ~Guard() { mz_zip_reader_end(z); }
    } guard{&za};
    Package p;
    std::vector<std::pair<std::string, util::Bytes>> raw;
    util::Bytes ctypes;
    bool have_ct = false;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) continue;
        if (st.m_is_directory) continue;
        std::size_t sz = 0;
        void* data = mz_zip_reader_extract_to_heap(&za, i, &sz, 0);
        if (!data) throw std::runtime_error(std::string("cannot inflate ") + st.m_filename + " (the file is damaged)");
        util::Bytes b((std::uint8_t*)data, (std::uint8_t*)data + sz);
        mz_free(data);
        std::string zname = st.m_filename;
        for (auto& c : zname) if (c == '\\') c = '/';
        if (util::iequals(zname, "[Content_Types].xml")) { ctypes = std::move(b); have_ct = true; continue; }
        raw.emplace_back("/" + zname, std::move(b));
    }
    if (!have_ct) throw std::runtime_error("package has no [Content_Types].xml");

    std::map<std::string, std::string> overrides;
    {
        pugi::xml_document d;
        if (!d.load_buffer(ctypes.data(), ctypes.size())) throw std::runtime_error("malformed [Content_Types].xml");
        for (auto e : d.document_element().children()) {
            std::string nm = e.name();
            if (auto c = nm.find(':'); c != std::string::npos) nm = nm.substr(c + 1);
            if (nm == "Default")
                p.defaults[util::lower(e.attribute("Extension").value())] = e.attribute("ContentType").value();
            else if (nm == "Override")
                overrides[key_of(e.attribute("PartName").value())] = e.attribute("ContentType").value();
        }
    }
    for (auto& [name, bytes] : raw) {
        if (auto src = rels_source(name)) {
            p.rels_[key_of(*src)] = parse_rels(bytes);
            continue;
        }
        std::string k = key_of(name);
        Part part;
        part.name = pct_decode(name);
        if (auto it = overrides.find(k); it != overrides.end()) part.content_type = it->second;
        else if (auto d = p.defaults.find(ext_of(name)); d != p.defaults.end()) part.content_type = d->second;
        part.bytes = std::move(bytes);
        p.order_.push_back(k);
        p.parts_.emplace(k, std::move(part));
    }
    return p;
}

util::Bytes Package::to_bytes() const {
    mz_zip_archive za{};
    if (!mz_zip_writer_init_heap(&za, 0, 1 << 16)) throw std::runtime_error("zip init failed");
    // ends the writer on every path (including throws below)
    struct Guard {
        mz_zip_archive* z;
        ~Guard() { mz_zip_writer_end(z); }
    } guard{&za};
    auto add = [&](const std::string& name, const std::string& data) {
        std::string zn = name[0] == '/' ? name.substr(1) : name;
        if (!mz_zip_writer_add_mem(&za, zn.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL))
            throw std::runtime_error("zip write failed for " + zn);
    };

    // [Content_Types].xml first, as Word writes it.
    {
        pugi::xml_document d;
        auto root = d.append_child("Types");
        root.append_attribute("xmlns") = NS_CT;
        std::map<std::string, std::string> defs = defaults;
        defs.try_emplace("rels", ct::rels);
        defs.try_emplace("xml", ct::xml);
        for (auto& [ext, ctype] : defs) {
            auto e = root.append_child("Default");
            e.append_attribute("Extension") = ext.c_str();
            e.append_attribute("ContentType") = ctype.c_str();
        }
        for (auto& k : order_) {
            auto it = parts_.find(k);
            if (it == parts_.end()) continue;
            const Part& part = it->second;
            if (part.content_type.empty()) continue;
            auto d2 = defs.find(ext_of(part.name));
            if (d2 != defs.end() && d2->second == part.content_type) continue;
            auto e = root.append_child("Override");
            e.append_attribute("PartName") = part.name.c_str();
            e.append_attribute("ContentType") = part.content_type.c_str();
        }
        add("[Content_Types].xml", xml_to_string(d));
    }
    if (auto it = rels_.find("/"); it != rels_.end() && !it->second.items.empty())
        add("/_rels/.rels", serialize_rels(it->second));
    for (auto& k : order_) {
        auto it = parts_.find(k);
        if (it == parts_.end()) continue;
        const Part& part = it->second;
        if (part.content_type.empty()) continue;  // not a part in OPC terms
        if (part.xml && part.dirty) add(part.name, xml_to_string(*part.xml));
        else add(part.name, std::string(part.bytes.begin(), part.bytes.end()));
        if (auto r = rels_.find(k); r != rels_.end() && !r->second.items.empty())
            add(rels_name_of(part.name), serialize_rels(r->second));
    }
    void* buf = nullptr;
    std::size_t sz = 0;
    if (!mz_zip_writer_finalize_heap_archive(&za, &buf, &sz)) throw std::runtime_error("zip finalize failed");
    // finalize hands the heap buffer to us; the writer no longer owns it
    util::Bytes out((std::uint8_t*)buf, (std::uint8_t*)buf + sz);
    mz_free(buf);
    return out;
}

void Package::save(const std::string& path) const { util::write_file_atomic(path, to_bytes()); }

bool Package::has(std::string_view name) const { return parts_.count(key_of(name)) > 0; }
Part* Package::part(std::string_view name) {
    auto it = parts_.find(key_of(name));
    return it == parts_.end() ? nullptr : &it->second;
}
const Part* Package::part(std::string_view name) const {
    auto it = parts_.find(key_of(name));
    return it == parts_.end() ? nullptr : &it->second;
}

pugi::xml_document& Package::xml(std::string_view name) {
    Part* p = part(name);
    if (!p) throw std::runtime_error("no such part: " + std::string(name));
    if (!p->xml) {
        p->xml = std::make_unique<pugi::xml_document>();
        auto r = p->xml->load_buffer(p->bytes.data(), p->bytes.size(),
                                     pugi::parse_default | pugi::parse_declaration | pugi::parse_ws_pcdata_single);
        if (!r) throw std::runtime_error("malformed XML in " + p->name + ": " + r.description());
    }
    p->dirty = true;  // callers that take the DOM may mutate it
    return *p->xml;
}

Part& Package::add_part(std::string name, std::string content_type, util::Bytes bytes) {
    std::string k = key_of(name);
    if (!parts_.count(k)) order_.push_back(k);
    Part& p = parts_[k];
    p.name = std::move(name);
    p.content_type = std::move(content_type);
    p.bytes = std::move(bytes);
    p.xml.reset();
    p.dirty = false;
    return p;
}

Part& Package::add_xml_part(std::string name, std::string content_type, std::string_view text) {
    Part& p = add_part(std::move(name), std::move(content_type), util::Bytes(text.begin(), text.end()));
    xml(p.name);
    if (normalizer) normalizer(*p.xml);
    return p;
}

void Package::remove_part(std::string_view name) {
    std::string k = key_of(name);
    parts_.erase(k);
    rels_.erase(k);
    order_.erase(std::remove(order_.begin(), order_.end(), k), order_.end());
}

std::vector<std::string> Package::part_names() const {
    std::vector<std::string> v;
    for (auto& k : order_) if (auto it = parts_.find(k); it != parts_.end()) v.push_back(it->second.name);
    return v;
}

Rels& Package::rels(std::string_view source) { return rels_[source == "/" ? std::string("/") : key_of(source)]; }
const Rels* Package::rels_if(std::string_view source) const {
    auto it = rels_.find(source == "/" ? std::string("/") : key_of(source));
    return it == rels_.end() ? nullptr : &it->second;
}

std::string Package::resolve(std::string_view source, std::string_view target) {
    std::string t(target);
    if (auto h = t.find('#'); h != std::string::npos) t = t.substr(0, h);
    t = pct_decode(t);
    std::vector<std::string> segs;
    if (t.empty() || t[0] != '/') {
        std::string base(source);
        auto slash = base.rfind('/');
        base = slash == std::string::npos ? "" : base.substr(0, slash);
        for (auto& s : util::split(base, '/')) if (!s.empty()) segs.push_back(s);
    }
    for (auto& s : util::split(t, '/')) {
        if (s.empty() || s == ".") continue;
        if (s == "..") { if (!segs.empty()) segs.pop_back(); continue; }
        segs.push_back(s);
    }
    std::string out;
    for (auto& s : segs) out += "/" + s;
    return out.empty() ? "/" : out;
}

std::string Package::relative(std::string_view source, std::string_view part_name) {
    auto sdir = util::split(source.substr(0, source.rfind('/')), '/');
    auto tseg = util::split(part_name, '/');
    std::vector<std::string> a, b;
    for (auto& s : sdir) if (!s.empty()) a.push_back(s);
    for (auto& s : tseg) if (!s.empty()) b.push_back(s);
    std::size_t i = 0;
    while (i < a.size() && i + 1 < b.size() && util::iequals(a[i], b[i])) i++;
    std::string out;
    for (std::size_t k = i; k < a.size(); k++) out += "../";
    for (std::size_t k = i; k < b.size(); k++) out += b[k] + (k + 1 < b.size() ? "/" : "");
    return out;
}

std::string Package::related(std::string_view source, std::string_view type) const {
    const Rels* r = rels_if(source);
    if (!r) return "";
    const Relationship* rel = r->first_of(type);
    if (!rel || rel->mode == TargetMode::External) return "";
    return resolve(source == "/" ? "/x" : source, rel->target);
}

std::string Package::main_part() const {
    std::string m = related("/", rt::officeDocument);
    if (m.empty()) {
        // Strict conformance uses a different relationship namespace.
        const Rels* r = rels_if("/");
        if (r)
            for (auto& rel : r->items)
                if (rel.type.ends_with("/officeDocument")) return resolve("/x", rel.target);
    }
    return m;
}

std::string Package::unique_name(std::string_view stem, std::string_view ext) const {
    for (int i = 1;; i++) {
        std::string n = std::string(stem) + std::to_string(i) + std::string(ext);
        if (!has(n)) return n;
    }
}

std::string Package::content_type_of(std::string_view name) const {
    const Part* p = part(name);
    return p ? p->content_type : "";
}

void Package::set_override(std::string name, std::string content_type) {
    if (Part* p = part(name)) p->content_type = std::move(content_type);
}

}  // namespace docx::opc
