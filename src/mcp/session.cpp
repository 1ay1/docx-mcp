#include "mcp/session.hpp"

#include <algorithm>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/fsio.hpp"

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

std::string Sessions::open(const std::string& path_in, bool create_missing) {
    std::lock_guard lk(mu);
    std::string path = canon(path_in);
    for (auto& [h, s] : by_handle_)
        if (same_path(s->path, path)) return h;
    auto s = std::make_unique<Session>();
    s->path = path;
    if (util::file_exists(path)) {
        s->doc = wml::Document::open(path);
        s->baseline_issues = count_issues(s->doc);
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
