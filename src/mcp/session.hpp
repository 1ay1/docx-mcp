// Open documents held by the server, addressed by a handle.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "wml/document.hpp"

namespace docx::server {

struct Session {
    std::string path;          // where it came from / saves to
    wml::Document doc;
    bool dirty = false;
    int baseline_issues = 0;  // schema issues present when opened
    std::vector<util::Bytes> undo;  // snapshots before each mutating call
};

class Sessions {
public:
    // Open (or return the already open) document at path. create=true makes
    // a new blank document when the file doesn't exist.
    std::string open(const std::string& path, bool create);
    std::string create(const std::string& path);
    Session& get(const std::string& handle_or_path);
    void close(const std::string& handle);
    std::vector<std::pair<std::string, Session*>> all();
    std::string default_author = "docx-mcp";
    std::recursive_mutex mu;

private:
    std::map<std::string, std::unique_ptr<Session>> by_handle_;
    int next_ = 1;
};

}  // namespace docx::server
