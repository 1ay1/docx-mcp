// Argument normalization: make the arguments any model sends fit the schema.
#include <map>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "mcp/tools.hpp"
#include "util/util.hpp"

namespace docx::server {

namespace {

std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// "paragraphId", "paragraph-id", "ParagraphID" -> "paragraph_id"
std::string snake(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '-' || c == ' ') { o += '_'; continue; }
        if (std::isupper((unsigned char)c)) {
            if (i > 0 && o.back() != '_' && !std::isupper((unsigned char)s[i - 1])) o += '_';
            o += char(std::tolower((unsigned char)c));
        } else {
            o += c;
        }
    }
    return o;
}

// Common names models use for our arguments.
const std::map<std::string, std::string>& aliases() {
    static const std::map<std::string, std::string> m = {
        {"document", "doc"}, {"doc_id", "doc"}, {"handle", "doc"}, {"file", "path"}, {"file_path", "path"},
        {"filename", "path"}, {"filepath", "path"}, {"para", "paragraph"}, {"paragraph_id", "paragraph"},
        {"para_id", "paragraph"}, {"pid", "paragraph"}, {"paragraph_ids", "paragraphs"}, {"ids_list", "ids"},
        {"content", "text"}, {"new_text", "text"}, {"replacement", "text"}, {"old_text", "match"},
        {"search", "match"}, {"find_text", "match"}, {"target_text", "match"}, {"tracked", "track"},
        {"track_changes", "track"}, {"comment", "text"}, {"comment_text", "text"}, {"style_name", "style"},
        {"style_id", "style"}, {"formatting", "format"}, {"fmt", "format"}, {"properties", "format"},
        {"url", "url"}, {"href", "url"}, {"link", "url"}, {"row_index", "row"}, {"col_index", "col"},
        {"column", "col"}, {"table_index", "table"}, {"table_id", "table"}, {"offset_start", "start"},
        {"offset_end", "end"}, {"pos", "position"}, {"index", "position"}, {"value_text", "value"},
        {"save_path", "path"}, {"output_path", "output"}, {"out", "output"}};
    return m;
}

std::string type_name(const Json& schema) {
    if (schema.contains("enum")) return "one of " + schema["enum"].dump();
    return schema.value("type", std::string("any"));
}

// Coerce v to the schema. Returns true if it changed.
bool coerce(const std::string& name, Json& v, const Json& sch) {
    std::string t = sch.value("type", std::string());
    if (v.is_null()) return false;
    auto bad = [&]() {
        throw std::runtime_error("argument '" + name + "' should be " + type_name(sch) + ", got " + v.dump());
    };
    if (sch.contains("enum")) {
        std::string s = v.is_string() ? v.get<std::string>() : v.dump();
        for (auto& e : sch["enum"])
            if (e.get<std::string>() == s) { if (!v.is_string()) { v = s; return true; } return false; }
        // case/spacing-insensitive match: "Bullet", "number list", "insert-below"
        auto key = [](std::string x) {
            std::string o;
            for (char c : x) if (std::isalnum((unsigned char)c)) o += char(std::tolower((unsigned char)c));
            return o;
        };
        for (auto& e : sch["enum"])
            if (key(e.get<std::string>()) == key(s)) { v = e; return true; }
        bad();
    }
    if (t == "string") {
        if (v.is_string()) return false;
        if (v.is_number_integer()) { v = std::to_string(v.get<long long>()); return true; }
        if (v.is_number()) {
            double d = v.get<double>();
            v = d == std::floor(d) ? std::to_string((long long)d) : v.dump();
            return true;
        }
        if (v.is_boolean()) { v = v.get<bool>() ? "true" : "false"; return true; }
        if (v.is_array() && v.size() == 1 && v[0].is_string()) { v = v[0]; return true; }
        bad();
    }
    if (t == "integer" || t == "number") {
        if (v.is_number()) {
            if (t == "integer" && !v.is_number_integer()) { v = (long long)std::llround(v.get<double>()); return true; }
            return false;
        }
        if (v.is_string()) {
            std::string s = util::trim(v.get<std::string>());
            try {
                std::size_t used = 0;
                double d = std::stod(s, &used);
                if (used == s.size()) {
                    if (t == "integer") v = (long long)std::llround(d);
                    else v = d;
                    return true;
                }
            } catch (...) {}
            // a number with a unit ("12pt", "0.5in") is meaningful for lengths; keep as string
            if (t == "number" && !s.empty()) return false;
        }
        if (v.is_boolean()) { v = v.get<bool>() ? 1 : 0; return true; }
        bad();
    }
    if (t == "boolean") {
        if (v.is_boolean()) return false;
        if (v.is_number()) { v = v.get<double>() != 0; return true; }
        if (v.is_string()) {
            std::string s = util::lower(util::trim(v.get<std::string>()));
            if (s == "true" || s == "yes" || s == "on" || s == "1" || s == "y") { v = true; return true; }
            if (s == "false" || s == "no" || s == "off" || s == "0" || s == "n" || s.empty()) { v = false; return true; }
        }
        bad();
    }
    if (t == "object") {
        if (v.is_object()) return false;
        if (v.is_string()) {
            std::string s = util::trim(v.get<std::string>());
            if (s.empty()) { v = Json::object(); return true; }
            try {
                Json j = Json::parse(s);
                if (j.is_object()) { v = j; return true; }
            } catch (...) {}
        }
        throw std::runtime_error("argument '" + name + "' should be a JSON object like {\"bold\": true}, got " + v.dump());
    }
    if (t == "array") {
        bool changed = false;
        if (v.is_string()) {
            std::string s = util::trim(v.get<std::string>());
            Json parsed;
            bool ok = false;
            if (!s.empty() && s.front() == '[') {
                try { parsed = Json::parse(s); ok = parsed.is_array(); } catch (...) {}
            }
            if (ok) v = parsed;
            else v = Json::array({v});
            changed = true;
        } else if (!v.is_array()) {
            v = Json::array({v});
            changed = true;
        }
        if (sch.contains("items")) {
            for (std::size_t i = 0; i < v.size(); i++)
                changed |= coerce(name + "[" + std::to_string(i) + "]", v[i], sch["items"]);
        }
        return changed;
    }
    return false;
}

}  // namespace

std::vector<std::string> profile_names() { return {"full", "core", "review"}; }

std::vector<ToolDef> select_tools(std::vector<ToolDef> all, const std::string& profile_in) {
    std::string profile = util::lower(util::trim(profile_in));
    if (profile.empty() || profile == "full" || profile == "all") return all;
    static const std::map<std::string, std::vector<std::string>> sets = {
        // for clients that rank or cap tools: the everyday ones, plus help and
        // run_tool so nothing else is out of reach
        {"essential", {"open_document", "create_document", "save_document", "read_document", "find_text",
                       "insert_text", "replace_text", "delete_text", "insert_paragraph", "insert_markdown", "delete_paragraph",
                       "format_text", "undo"}},
        {"core", {"open_document", "create_document", "save_document", "close_document", "undo",
                  "read_document", "get_text", "get_outline", "find_text",
                  "insert_text", "replace_text", "delete_text", "find_replace", "insert_paragraph", "insert_markdown", "delete_paragraph",
                  "format_text", "format_paragraph", "apply_style", "apply_list",
                  "set_track_changes", "list_revisions", "accept_revisions", "reject_revisions",
                  "add_comment", "list_comments", "reply_comment", "resolve_comment",
                  "insert_table", "set_cell", "insert_picture", "set_header_footer", "set_page_setup"}},
        {"review", {"open_document", "save_document", "close_document", "undo", "read_document", "get_text", "find_text",
                    "get_paragraph", "set_track_changes", "insert_text", "replace_text", "delete_text", "find_replace",
                    "format_text", "list_revisions", "accept_revisions", "reject_revisions", "compare_documents",
                    "add_comment", "list_comments", "reply_comment", "resolve_comment", "edit_comment", "delete_comment",
                    "protect_document", "unprotect_document"}}};
    std::vector<std::string> names;
    if (auto it = sets.find(profile); it != sets.end()) names = it->second;
    else
        for (auto& n : util::split(profile, ',')) if (!util::trim(n).empty()) names.push_back(util::trim(n));
    std::vector<ToolDef> out;
    for (auto& n : names) {
        bool found = false;
        for (auto& t : all)
            if (t.name == n) { out.push_back(t); found = true; break; }
        if (!found) throw std::runtime_error("unknown tool or profile '" + n + "' (profiles: full, essential, core, review)");
    }
    // every profile keeps the way to the rest
    for (auto n : {"help", "run_tool"}) {
        bool have = false;
        for (auto& t : out) have |= t.name == n;
        if (!have)
            for (auto& t : all)
                if (t.name == n) out.push_back(t);
    }
    return out;
}

std::vector<std::string> normalize_args(const ToolDef& t, Json& args) {
    std::vector<std::string> notes;
    if (args.is_null()) args = Json::object();
    if (args.is_string()) {
        try { args = Json::parse(args.get<std::string>()); } catch (...) {}
    }
    if (!args.is_object()) throw std::runtime_error("arguments must be a JSON object");
    const Json& props = t.schema.contains("properties") ? t.schema["properties"] : Json::object();
    // 1. rename unknown keys: exact alias, snake_case form, or a close name
    Json out = Json::object();
    for (auto& [k, v] : args.items()) {
        if (props.contains(k)) {
            if (out.contains(k) && out[k].is_object() && v.is_object()) out[k].update(v);  // folded keys came first
            else out[k] = v;
            continue;
        }
        std::string target;
        std::string sk = snake(k);
        if (props.contains(sk)) target = sk;
        else if (auto it = aliases().find(sk); it != aliases().end() && props.contains(it->second)) target = it->second;
        else {
            std::size_t best = 99;
            for (auto& [p, _] : props.items()) {
                std::size_t dist = edit_distance(sk, p);
                if (dist < best) { best = dist; target = p; }
            }
            if (best > std::max<std::size_t>(1, sk.size() / 4)) target.clear();
        }
        if (target.empty()) {
            // a key that belongs inside an object argument ("margins" given
            // flat instead of inside setup): move it there
            std::string owner;
            for (auto& [p, ps] : props.items())
                if (ps.value("type", "") == "object" && ps.contains("properties") && ps["properties"].contains(sk)) {
                    owner = p;
                    break;
                }
            if (!owner.empty()) {
                Json& o = out[owner];
                if (!o.is_object()) o = Json::object();
                if (!o.contains(sk)) o[sk] = v;
                notes.push_back("moved '" + k + "' into " + owner);
                continue;
            }
            std::string names;
            for (auto& [p, _] : props.items()) names += (names.empty() ? "" : ", ") + p;
            // A read tool loses nothing by dropping a guess ("xml_hint" on
            // get_paragraph): answer, and say what was ignored. A tool that
            // changes the document refuses instead: the stray argument may be
            // the change the caller wanted, and half-doing it is worse.
            if (t.read_only) {
                notes.push_back("ignored '" + k + "', which " + t.name + " doesn't take (it takes: " + names + ")");
                continue;
            }
            throw std::runtime_error(t.name + " has no argument '" + k + "'. Valid arguments: " + names);
        }
        if (out.contains(target) || args.contains(target)) continue;  // the real key wins
        out[target] = v;
        notes.push_back("treated '" + k + "' as '" + target + "'");
    }
    // 2. coerce types
    for (auto& [k, v] : out.items()) {
        if (!props.contains(k)) continue;
        coerce(k, v, props[k]);
    }
    // 3. required
    for (auto& r : t.schema.value("required", Json::array())) {
        std::string k = r.get<std::string>();
        if (!out.contains(k) || out[k].is_null() || (out[k].is_string() && out[k].get<std::string>().empty()))
            throw std::runtime_error("missing required argument '" + k + "' for " + t.name +
                                     (props.contains(k) && props[k].contains("description")
                                          ? " (" + props[k]["description"].get<std::string>() + ")"
                                          : ""));
    }
    args = out;
    return notes;
}

Json call_tool(const ToolDef& t, Sessions& S, Json args) {
    auto notes = normalize_args(t, args);
    Json out = t.run(S, args);
    if (!out.is_object()) out = Json{{"result", out}};
    if (!notes.empty()) out["_notes"] = notes;
    return out;
}

}  // namespace docx::server
