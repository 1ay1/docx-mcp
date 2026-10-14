#include "mcp/session.hpp"

#include <filesystem>
#include <stdexcept>

#include "schema/schema.hpp"

namespace docx::server {

namespace fs = std::filesystem;

namespace {
std::string canon(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(fs::absolute(p), ec);
    return ec ? p : c.string();
}
}  // namespace

std::string Sessions::open(const std::string& path_in, bool create_missing) {
    std::lock_guard lk(mu);
    std::string path = canon(path_in);
    for (auto& [h, s] : by_handle_)
        if (s->path == path) return h;
    auto s = std::make_unique<Session>();
    s->path = path;
    if (fs::exists(path)) {
        s->doc = wml::Document::open(path);
        for (auto& part : s->doc.pkg().part_names())
            if (schema::part_root_type(s->doc.pkg().content_type_of(part)) >= 0)
                s->baseline_issues += (int)schema::validate(s->doc.pkg().xml(part).document_element(), {100000}).size();
    }
    else if (create_missing) { s->doc = wml::Document::create(); s->dirty = true; }
    else throw std::runtime_error("no such file: " + path);
    s->doc.author.name = default_author;
    std::string h = "doc" + std::to_string(next_++);
    by_handle_[h] = std::move(s);
    return h;
}

std::string Sessions::create(const std::string& path_in) {
    std::lock_guard lk(mu);
    std::string path = canon(path_in);
    for (auto& [h, s] : by_handle_)
        if (s->path == path) throw std::runtime_error(path + " is already open as " + h);
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
    std::string path = canon(key);
    for (auto& [h, s] : by_handle_)
        if (s->path == path) return *s;
    // a path that isn't open yet: open it on demand
    if (fs::exists(path)) return *by_handle_[open(path, false)];
    throw std::runtime_error("no open document '" + key + "' (open_document first, or pass a file path)");
}

void Sessions::close(const std::string& key) {
    std::lock_guard lk(mu);
    for (auto it = by_handle_.begin(); it != by_handle_.end(); ++it)
        if (it->first == key || it->second->path == canon(key)) { by_handle_.erase(it); return; }
    throw std::runtime_error("no open document '" + key + "'");
}

std::vector<std::pair<std::string, Session*>> Sessions::all() {
    std::lock_guard lk(mu);
    std::vector<std::pair<std::string, Session*>> v;
    for (auto& [h, s] : by_handle_) v.emplace_back(h, s.get());
    return v;
}

}  // namespace docx::server
