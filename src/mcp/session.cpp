#include "mcp/session.hpp"

#include <algorithm>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/fsio.hpp"
#include "wml/render.hpp"
#include <set>

namespace docx::server {

namespace {
std::string canon(const std::string& p) { return util::normalize_path(p); }

bool same_path(const std::string& a, const std::string& b) {
#if defined(_WIN32) || defined(__APPLE__)
    return util::iequals(a, b);  // case-insensitive file systems by default
#else
    return a == b;
#endif
}

int count_issues(wml::Document& d) {
    int n = 0;
    for (auto& part : d.pkg().part_names())
        if (schema::part_root_type(d.pkg().content_type_of(part)) >= 0)
            n += (int)schema::validate(d.pkg().xml(part).document_element(), {100000}).size();
    return n;
}
}  // namespace

int Session::baseline() {
    if (baseline_issues < 0) {
        if (opened.empty()) baseline_issues = 0;
        else {
            auto d = wml::Document::from_bytes(opened);
            baseline_issues = count_issues(d);
            opened.clear();
        }
    }
    return baseline_issues;
}

std::string Sessions::open(const std::string& path_in, bool create_missing) {
    std::lock_guard lk(mu);
    std::string path = canon(path_in);
    for (auto& [h, s] : by_handle_)
        if (same_path(s->path, path)) return h;
    auto s = std::make_unique<Session>();
    s->path = path;
    if (util::file_exists(path)) {
        s->opened = util::read_file(path);
        // legacy formats (.doc, .rtf, .odt, ...) convert to .docx; saving writes a new .docx next to it
        std::string ext = util::lower(util::path_utf8(util::u8path(path).extension()));
        static const std::set<std::string> legacy = {".doc", ".rtf", ".odt", ".wpd", ".wps", ".dot", ".fodt", ".sxw", ".abw", ".htm", ".html", ".txt", ".xml"};
        bool flat = opc::Package::is_flat_opc(s->opened);
        // .odt and .sxw are zips too, so the extension decides; an .xml that isn't Flat OPC is Word 2003 XML
        bool convert = legacy.count(ext) && !flat;
        if (convert && (ext == ".doc" || ext == ".dot") && s->opened.size() > 2 && s->opened[0] == 'P' && s->opened[1] == 'K')
            convert = false;  // a .docx with the wrong name
        if (convert) {
            s->opened = wml::convert_to_docx(path);
            // a fresh .docx name: never overwrite a file that's already there
            auto p = util::u8path(path);
            p.replace_extension(".docx");
            std::string target = util::path_utf8(p);
            for (int k = 2; util::file_exists(target); k++) {
                auto q = util::u8path(path);
                q.replace_filename(util::path_utf8(q.stem()) + " (" + std::to_string(k) + ").docx");
                target = util::path_utf8(q);
            }
            for (auto& [h, o] : by_handle_)
                if (same_path(o->path, target)) throw std::runtime_error(target + " is already open as " + h);
            s->path = target;
            s->converted_from = path;
            s->dirty = true;
        }
        s->doc = wml::Document::from_bytes(s->opened);
        s->baseline_issues = -1;  // counted on first need (only when a save finds issues)
    } else if (create_missing) {
        s->doc = wml::Document::create();
        s->dirty = true;
    } else {
        throw std::runtime_error("file not found: " + path);
    }
    s->doc.author.name = default_author;
    std::string h = "doc" + std::to_string(next_++);
    by_handle_[h] = std::move(s);
    return h;
}

std::string Sessions::create(const std::string& path_in) {
    std::lock_guard lk(mu);
    std::string path = canon(path_in);
    for (auto& [h, s] : by_handle_)
        if (same_path(s->path, path)) throw std::runtime_error(path + " is already open as " + h);
    auto s = std::make_unique<Session>();
    s->path = path;
    s->doc = wml::Document::create();
    s->doc.author.name = default_author;
    s->dirty = true;
    std::string h = "doc" + std::to_string(next_++);
    by_handle_[h] = std::move(s);
    return h;
}

Session& Sessions::get(const std::string& key) {
    std::lock_guard lk(mu);
    if (auto it = by_handle_.find(key); it != by_handle_.end()) return *it->second;
    // models sometimes send "Doc1", "1" or the handle with spaces
    std::string k = util::lower(util::trim(key));
    if (!k.empty() && std::all_of(k.begin(), k.end(), [](char c) { return c >= '0' && c <= '9'; })) k = "doc" + k;
    if (auto it = by_handle_.find(k); it != by_handle_.end()) return *it->second;
    std::string path = canon(key);
    for (auto& [h, s] : by_handle_)
        if (same_path(s->path, path)) return *s;
    // a path that isn't open yet: open it on demand
    if (util::file_exists(path)) return *by_handle_[open(path, false)];
    std::string open_list;
    for (auto& [h, s] : by_handle_) open_list += (open_list.empty() ? "" : ", ") + h + " (" + s->path + ")";
    throw std::runtime_error("no open document '" + key + "'. " +
                             (open_list.empty() ? std::string("Nothing is open; call open_document or create_document first.")
                                                : "Open documents: " + open_list + "."));
}

void Sessions::close(const std::string& key) {
    std::lock_guard lk(mu);
    Session* target = &get(key);
    for (auto it = by_handle_.begin(); it != by_handle_.end(); ++it)
        if (it->second.get() == target) { by_handle_.erase(it); return; }
}

std::vector<std::pair<std::string, Session*>> Sessions::all() {
    std::lock_guard lk(mu);
    std::vector<std::pair<std::string, Session*>> v;
    for (auto& [h, s] : by_handle_) v.emplace_back(h, s.get());
    return v;
}

}  // namespace docx::server
