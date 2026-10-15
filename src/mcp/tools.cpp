#include "mcp/discover.hpp"
#include "mcp/tools.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "schema/schema.hpp"
#include "util/util.hpp"
#include "wml/comments.hpp"
#include "wml/docops.hpp"
#include "wml/document.hpp"
#include "wml/render.hpp"
#include "wml/edit.hpp"
#include "wml/markdown.hpp"
#include "wml/structure.hpp"
#include "wml/styles.hpp"
#include "wml/text.hpp"
#include "util/fsio.hpp"
#include "xml/xml.hpp"

namespace docx::server {

using namespace docx::wml;
namespace fs = std::filesystem;

namespace {

// ── schema helpers ───────────────────────────────────────────────────────

Json obj(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}};
    if (!required.empty()) s["required"] = required;
    s["additionalProperties"] = false;
    return s;
}
Json str(const std::string& d) { return {{"type", "string"}, {"description", d}}; }
Json integer(const std::string& d) { return {{"type", "integer"}, {"description", d}}; }
Json num(const std::string& d) { return {{"type", "number"}, {"description", d}}; }
Json boolean(const std::string& d) { return {{"type", "boolean"}, {"description", d}}; }
Json anyobj(const std::string& d) { return {{"type", "object"}, {"description", d}}; }
Json enm(std::vector<std::string> v, const std::string& d) { return {{"type", "string"}, {"enum", v}, {"description", d}}; }
Json arr(Json items, const std::string& d) { return {{"type", "array"}, {"items", items}, {"description", d}}; }

const Json kDoc = str("Handle from open_document (e.g. \"doc1\") or the .docx path.");
const Json kPara = str("Paragraph `id` from read_document (e.g. \"1A2B3C4D\") or its number (\"3\").");
const Json kTrack = boolean("Record as a tracked change (default: the document's setting).");
// Formatting objects: the common keys are typed (so models get them right);
// rarer ones are allowed too (additionalProperties) and listed in the docs.
Json fmt_schema(const char* desc, Json props) {
    return {{"type", "object"}, {"description", desc}, {"properties", props}, {"additionalProperties", true}};
}
const Json kRunFmt = fmt_schema(
    "Character formatting (Word names). null removes a property. More keys: caps, small_caps, double_strike, hidden, "
    "spacing, position, scale, kerning, language, border, shading, emboss, outline, shadow.",
    {{"bold", {{"type", "boolean"}}}, {"italic", {{"type", "boolean"}}},
     {"underline", {{"type", "string"}, {"description", "single, double, thick, dotted, wave, none"}}},
     {"strike", {{"type", "boolean"}}}, {"font", {{"type", "string"}}},
     {"size", {{"type", "number"}, {"description", "points"}}},
     {"color", {{"type", "string"}, {"description", "hex RRGGBB or a name"}}},
     {"highlight", {{"type", "string"}, {"description", "yellow, green, cyan, red, ... or none"}}},
     {"superscript", {{"type", "boolean"}}}, {"subscript", {{"type", "boolean"}}},
     {"style", {{"type", "string"}, {"description", "character style, e.g. Strong"}}}});
const Json kLen = {{"type", Json::array({"number", "string"})}, {"description", "points, or with a unit: \"0.5in\", \"1cm\", \"6pt\""}};
const Json kParaFmt = fmt_schema(
    "Paragraph formatting (Word names). Lengths in points or with a unit (\"0.5in\", \"1cm\"). null removes. More keys: "
    "line_rule, first_line_indent, hanging_indent, keep_together, page_break_before, widow_control, outline_level, "
    "shading, borders, tabs, bidi.",
    {{"style", {{"type", "string"}, {"description", "e.g. \"Heading 1\", \"Title\", \"Quote\""}}},
     {"alignment", {{"type", "string"}, {"description", "left, center, right, both (justified)"}}},
     {"space_before", kLen}, {"space_after", kLen},
     {"line_spacing", {{"type", Json::array({"number", "string"})}, {"description", "multiple like 1.15 or exact like \"14pt\""}}},
     {"indent_left", kLen}, {"indent_right", kLen},
     {"keep_with_next", {{"type", "boolean"}}}});

// ── argument helpers ─────────────────────────────────────────────────────
// Models differ in how they send arguments: numbers as strings, booleans as
// "true"/"yes", one id instead of a list, objects as JSON text. Accept all of
// those; reject only what can't mean anything.

// An object passed as a JSON string ("{\"bold\":true}") is decoded.
Json as_obj(const Json& v, const char* k) {
    if (v.is_object()) return v;
    if (v.is_null()) return Json::object();
    if (v.is_string()) {
        std::string s = util::trim(v.get<std::string>());
        if (s.empty()) return Json::object();
        try {
            Json j = Json::parse(s);
            if (j.is_object()) return j;
        } catch (...) {}
    }
    throw std::runtime_error(std::string("argument '") + k + "' must be an object, e.g. {\"bold\": true}");
}

Json obj_arg(const Json& a, const char* k) { return a.contains(k) ? as_obj(a[k], k) : Json::object(); }

Json arg(const Json& a, const char* k) {
    if (!a.contains(k)) return nullptr;
    return a[k];
}

std::string sarg(const Json& a, const char* k, const std::string& def = "") {
    Json v = arg(a, k);
    if (v.is_null()) return def;
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        double d = v.get<double>();
        if (d == (long long)d) return std::to_string((long long)d);
    }
    return v.dump();
}
int iarg(const Json& a, const char* k, int def) {
    Json v = arg(a, k);
    if (v.is_null()) return def;
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return (int)std::lround(v.get<double>());
    if (v.is_string()) {
        std::string s = util::trim(v.get<std::string>());
        if (s.empty()) return def;
        try {
            std::size_t used = 0;
            double d = std::stod(s, &used);
            if (used == s.size()) return (int)std::lround(d);
        } catch (...) {}
    }
    throw std::runtime_error(std::string("argument '") + k + "' must be a whole number, got " + v.dump());
}
double darg(const Json& a, const char* k, double def) {
    Json v = arg(a, k);
    if (v.is_null()) return def;
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) {}
    }
    throw std::runtime_error(std::string("argument '") + k + "' must be a number, got " + v.dump());
}
bool barg(const Json& a, const char* k, bool def) {
    Json v = arg(a, k);
    if (v.is_null()) return def;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) {
        std::string s = util::lower(util::trim(v.get<std::string>()));
        if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
        if (s == "false" || s == "no" || s == "off" || s == "0" || s.empty()) return false;
    }
    throw std::runtime_error(std::string("argument '") + k + "' must be true or false, got " + v.dump());
}
std::string need(const Json& a, const char* k) {
    if (!a.contains(k) || a[k].is_null() || (a[k].is_string() && a[k].get<std::string>().empty()))
        throw std::runtime_error(std::string("missing required argument '") + k + "'");
    return sarg(a, k);
}
EditOpts track(const Json& a) {
    EditOpts o;
    if (a.contains("track") && !a["track"].is_null()) o.track = barg(a, "track", false);
    return o;
}

Session& sess(Sessions& S, const Json& a) {
    auto& s = S.get(need(a, "doc"));
    // per-call override; the wrapper in add() restores the session author afterwards
    if (a.contains("author") && a["author"].is_string() && !sarg(a, "author").empty()) s.doc.author.name = sarg(a, "author");
    return s;
}

// The session a mutating call is working on, so a failure can roll it back.
thread_local Session* g_txn = nullptr;
thread_local bool g_txn_snap = false;

// Every mutating tool snapshots first so undo works, and so a failed call
// leaves the document as it was.
Session& mut(Sessions& S, const Json& a) {
    auto& s = sess(S, a);
    s.undo.push_back(s.doc.snapshot());
    // 30 steps, and at most ~512 MB of snapshots (always keep the newest)
    std::size_t total = 0;
    for (auto& u : s.undo) total += u.size();
    while (s.undo.size() > 1 && (s.undo.size() > 30 || total > (512u << 20))) {
        total -= s.undo.front().size();
        s.undo.erase(s.undo.begin());
    }
    if (!g_txn) { g_txn = &s; g_txn_snap = true; }
    s.dirty = true;
    return s;
}

ParaRef para(Session& s, const Json& a, const char* key = "paragraph") { return find_para(s.doc, need(a, key)); }

int vlen(pugi::xml_node p) { return (int)analyze(p).vis.size(); }

// Resolve a target range inside a paragraph: either offsets, or the n-th
// occurrence of `match` text. Returns {start, end}.
// The block to append after when no paragraph is given: the body's last
// paragraph or table (an empty body gets a paragraph, as Word keeps one).
pugi::xml_node last_block(Document& d) {
    auto bs = blocks(d.body());
    if (!bs.empty()) return bs.back();
    return d.body().insert_child_before("w:p", d.body().child("w:sectPr"));
}

std::pair<int, int> range_in(pugi::xml_node p, const Json& a) {
    int n = vlen(p);
    if (a.contains("match") && !a["match"].is_null()) {
        std::u32string m = util::to_u32(sarg(a, "match"));
        auto t = analyze(p).text;
        int occ = iarg(a, "occurrence", 1);
        std::size_t pos = 0;
        for (int k = 0;; k++) {
            pos = t.find(m, k == 0 ? 0 : pos + 1);
            if (pos == std::u32string::npos)
                throw std::runtime_error("text \"" + sarg(a, "match") + "\" not found " +
                                         (occ > 1 ? "(occurrence " + std::to_string(occ) + ")" : "") + " in that paragraph");
            if (k + 1 == occ) break;
        }
        return {(int)pos, (int)(pos + m.size())};
    }
    int s = iarg(a, "start", 0);
    int e = iarg(a, "end", n);
    if (s < 0) s = std::max(0, n + s + 1);
    if (e < 0) e = std::max(0, n + e + 1);
    if (s > n || e > n || s > e) throw std::runtime_error("range " + std::to_string(s) + ".." + std::to_string(e) + " outside paragraph (length " + std::to_string(n) + ")");
    return {s, e};
}

int pos_in(pugi::xml_node p, const Json& a) {
    int n = vlen(p);
    if (a.contains("after") && !a["after"].is_null()) {
        Json m = {{"match", a["after"]}, {"occurrence", iarg(a, "occurrence", 1)}};
        return range_in(p, m).second;
    }
    if (a.contains("before") && !a["before"].is_null()) {
        Json m = {{"match", a["before"]}, {"occurrence", iarg(a, "occurrence", 1)}};
        return range_in(p, m).first;
    }
    int pos = iarg(a, "position", n);
    if (pos < 0) pos = n + pos + 1;
    if (pos < 0 || pos > n) throw std::runtime_error("position outside paragraph (length " + std::to_string(n) + ")");
    return pos;
}

Json para_brief(Document& d, pugi::xml_node p) {
    Json j = {{"id", para_id(p)}, {"text", analyze(p).utf8()}};
    std::string sty = xml::attr(p.child("w:pPr").child("w:pStyle"), "w:val");
    if (!sty.empty()) j["style"] = sty;
    return j;
}

std::vector<std::string> strs(const Json& a, const char* k) {
    std::vector<std::string> v;
    if (!a.contains(k) || a[k].is_null()) return v;
    const Json& x = a[k];
    auto one = [](const Json& e) { return e.is_string() ? e.get<std::string>() : e.is_number_integer() ? std::to_string(e.get<long long>()) : e.dump(); };
    if (x.is_string()) {
        // "[\"a\",\"b\"]" or "a, b" or a single value
        std::string s = util::trim(x.get<std::string>());
        if (!s.empty() && s.front() == '[') {
            try {
                for (auto& e : Json::parse(s)) v.push_back(one(e));
                return v;
            } catch (...) {}
        }
        if (s.find(',') != std::string::npos && k != std::string("cells"))
            for (auto& p : util::split(s, ',')) { if (!util::trim(p).empty()) v.push_back(util::trim(p)); }
        else v.push_back(s);
        return v;
    }
    if (x.is_array()) for (auto& e : x) v.push_back(one(e));
    else v.push_back(one(x));
    return v;
}

util::Bytes load_file(const std::string& path) { return util::read_file(util::normalize_path(path)); }

Json validate_doc(Document& d) {
    Json issues = Json::array();
    int total = 0;
    for (auto& part : d.pkg().part_names()) {
        if (schema::part_root_type(d.pkg().content_type_of(part)) < 0) continue;
        for (auto& is : schema::validate(d.pkg().xml(part).document_element())) {
            total++;
            if (issues.size() < 50) issues.push_back({{"part", part}, {"path", is.path}, {"message", is.message}});
        }
    }
    // package-level checks: every relationship target exists
    for (auto& part : d.pkg().part_names()) {
        if (auto r = d.pkg().rels_if(part))
            for (auto& rel : r->items) {
                if (rel.mode == opc::TargetMode::External) continue;
                auto t = opc::Package::resolve(part, rel.target);
                if (!d.pkg().has(t)) {
                    total++;
                    issues.push_back({{"part", part}, {"path", rel.id}, {"message", "relationship target " + t + " is missing"}});
                }
            }
    }
    return {{"valid", total == 0}, {"issue_count", total}, {"issues", issues}};
}

}  // namespace

std::string server_instructions() {
    return
        "docx-mcp edits Word documents (.docx) the way Word does. Output is always a valid Word file.\n"
        "\n"
        "Steps:\n"
        "1. open_document {path} (or create_document {path}). It returns a handle like \"doc1\". "
        "Any tool also accepts the file path as `doc`.\n"
        "2. read_document {doc}. It lists paragraphs with an `id` (8 hex digits, e.g. \"1A2B3C4D\") and `n` (1, 2, 3...). "
        "Pass either one as `paragraph`. Prefer `id`: it stays valid after other edits.\n"
        "3. Edit. To target text inside a paragraph, give `match` (the exact text) instead of character offsets.\n"
        "4. save_document {doc}. Nothing is written to disk before this.\n"
        "\n"
        "Track changes: set_track_changes {doc, on: true, author} makes every later edit a Word revision. "
        "Review with list_revisions, accept_revisions, reject_revisions. Comments: add_comment, reply_comment, resolve_comment.\n"
        "Formatting uses Word's names: bold, italic, size (points), color (hex like FF0000), alignment (left/center/right/both), "
        "space_after (points), style (\"Heading 1\", \"Title\", \"Quote\"...).\n"
        "If something goes wrong, the error says how to fix the call. undo reverts the last change.\n"
        "\n"
        "There are tools for everything in Word's ribbon: tables, pictures, lists, styles, headers and footers, page "
        "setup, footnotes, links, fields and TOC, content controls, comments, tracked changes, compare, protection. "
        "If your client shows you only some of them, call help {topic} to find the one you need and run_tool "
        "{tool, arguments} to run it.";
}

std::vector<ToolDef> make_tools() {
    std::vector<ToolDef> T;
    auto add = [&](ToolDef t) {
        // a short description for every argument, or none rather than an empty one
        static const std::map<std::string, std::string> common = {
            {"match", "Exact text to act on."}, {"occurrence", "Which occurrence of match (default 1)."},
            {"start", "Start character offset."}, {"end", "End character offset."},
            {"position", "Character offset to insert at (default end)."},
            {"after", "Insert after this text."}, {"before", "Insert before this text."},
            {"paragraphs", "Paragraph ids or numbers."}, {"text", "The text."}, {"style", "Style name or id."},
            {"table", "Table number (1 = first table)."}, {"row", "Row number (1-based)."}, {"col", "Column number (1-based)."},
            {"name", "Name."}, {"comment_id", "Comment id from list_comments."}, {"id", "Id."},
            {"kind", "Kind."}, {"cells", "Cell texts."}, {"password", "Password."}, {"ids", "Ids."},
            {"by_author", "Only this author's changes."}, {"where", "after or before."}};
        if (t.schema.contains("properties"))
            for (auto& [k, p] : t.schema["properties"].items()) {
                if (!p.is_object() || !p.contains("description")) continue;
                if (!p["description"].get<std::string>().empty()) continue;
                if (auto it = common.find(k); it != common.end()) p["description"] = it->second;
                else p.erase("description");
            }
        // A failed call leaves the document as it was, and a per-call author
        // doesn't stick to the session.
        t.run = [run = std::move(t.run), sets_author = t.name == "open_document" || t.name == "create_document"](
                    Sessions& S, const Json& a) -> Json {
            Session* target = nullptr;
            wml::Author author;
            bool dirty = false;
            if (a.contains("doc") && a["doc"].is_string()) {
                try { target = &S.get(a["doc"].get<std::string>()); } catch (...) {}
                if (target) { author = target->doc.author; dirty = target->dirty; }
            }
            auto alive = [&](Session* s) {
                for (auto& [h, x] : S.all()) if (x == s) return true;
                return false;
            };
            g_txn = nullptr;
            g_txn_snap = false;
            try {
                Json out = run(S, a);
                // splitting a revision copies its id; Word gives each piece its own
                if (g_txn && g_txn_snap && alive(g_txn)) g_txn->doc.dedupe_ids();
                if (target && alive(target) && !sets_author) target->doc.author = author;
                g_txn = nullptr;
                return out;
            } catch (...) {
                if (g_txn && g_txn_snap && alive(g_txn) && !g_txn->undo.empty()) {
                    auto keep = g_txn->doc.author;
                    g_txn->doc = Document::from_bytes(g_txn->undo.back());
                    g_txn->doc.author = keep;
                    g_txn->undo.pop_back();
                    if (g_txn == target) g_txn->dirty = dirty;
                }
                if (target && alive(target)) target->doc.author = author;
                g_txn = nullptr;
                throw;
            }
        };
        T.push_back(std::move(t));
    };

    // ── documents ────────────────────────────────────────────────────────
    add({"open_document", "Open document",
         "Open a .docx file for reading/editing. Returns a handle. Opening an already open path returns the same handle.",
         obj({{"path", str("Path to the .docx file.")}, {"author", str("Name used for tracked changes and comments (default from server).")},
              {"create_if_missing", boolean("Create a blank document when the file doesn't exist.")}},
             {"path"}),
         false, false, [](Sessions& S, const Json& a) {
             std::string h = S.open(need(a, "path"), barg(a, "create_if_missing", false));
             auto& s = S.get(h);
             if (a.contains("author")) s.doc.author.name = sarg(a, "author");
             auto st = statistics(s.doc);
             return Json{{"doc", h}, {"path", s.path}, {"paragraphs", st["paragraphs"]}, {"words", st["words"]},
                         {"tables", st["tables"]}, {"comments", st["comments"]}, {"revisions", st["revisions"]},
                         {"track_changes", s.doc.track_changes()}, {"protection", protection_info(s.doc)}};
         }});
    add({"create_document", "New blank document",
         "Create a new blank document exactly like Word's File > New > Blank document (Calibri 11, Office theme, Letter). "
         "Saved to `path` on save_document.",
         obj({{"path", str("Where save_document will write it.")}, {"author", str("Author for revisions/comments and document properties.")},
              {"title", str("Document title property.")}, {"paper", enm({"letter", "a4", "legal", "a5", "a3"}, "Paper size (default letter).")}},
             {"path"}),
         false, false, [](Sessions& S, const Json& a) {
             std::string h = S.create(need(a, "path"));
             auto& s = S.get(h);
             if (a.contains("author")) {
                 s.doc.author.name = sarg(a, "author");
                 set_core_properties(s.doc, {{"creator", s.doc.author.name}, {"last_modified_by", s.doc.author.name}});
             }
             if (a.contains("title")) set_core_properties(s.doc, {{"title", a["title"]}});
             if (a.contains("paper")) format_section(s.doc, 1, {{"paper", a["paper"]}}, {false});
             return Json{{"doc", h}, {"path", s.path}};
         }});
    add({"save_document", "Save",
         "Write the document to disk (atomic replace). `path` saves a copy elsewhere (Save As) and retargets the handle.",
         obj({{"doc", kDoc}, {"path", str("Save As path (optional).")}, {"validate", boolean("Refuse to save if the result is not schema-valid (default true).")}}, {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             std::string path = sarg(a, "path", s.path);
             if (barg(a, "validate", true)) {
                 // refuse to write invalid markup we introduced; issues already in the
                 // file when it was opened don't block saving
                 auto v = validate_doc(s.doc);
                 if (!v["valid"].get<bool>() && v["issue_count"].get<int>() > s.baseline_issues)
                     throw std::runtime_error("not saved: document has schema issues: " + v["issues"].dump());
             }
             path = util::normalize_path(path);
             s.doc.save(path);
             s.path = path;
             s.dirty = false;
             return Json{{"saved", s.path}, {"bytes", util::file_size(s.path)}};
         }});
    add({"close_document", "Close", "Close a document without saving.", obj({{"doc", kDoc}}, {"doc"}), false, false,
         [](Sessions& S, const Json& a) {
             bool dirty = S.get(need(a, "doc")).dirty;
             S.close(need(a, "doc"));
             return Json{{"closed", true}, {"discarded_unsaved_changes", dirty}};
         }});
    add({"list_open_documents", "Open documents", "List the documents currently open in this server.", obj({}), true, false,
         [](Sessions& S, const Json&) {
             Json v = Json::array();
             for (auto& [h, s] : S.all()) v.push_back({{"doc", h}, {"path", s->path}, {"unsaved_changes", s->dirty}});
             return v;
         }});
    add({"undo", "Undo", "Undo the last change (repeatable, up to 30 steps).", obj({{"doc", kDoc}, {"steps", integer("How many (default 1).")}}, {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             int n = iarg(a, "steps", 1), done = 0;
             auto author = s.doc.author;
             while (n-- > 0 && !s.undo.empty()) {
                 s.doc = Document::from_bytes(s.undo.back());
                 s.undo.pop_back();
                 done++;
             }
             s.doc.author = author;
             return Json{{"undone", done}, {"remaining", s.undo.size()}};
         }});

    // ── reading ──────────────────────────────────────────────────────────
    add({"read_document", "Read",
         "Read paragraphs with their ids, styles, list labels, table positions and revision/comment markers. "
         "Paginate with offset/limit. view=original shows the text before tracked changes.",
         obj({{"doc", kDoc}, {"offset", integer("1-based first paragraph (negative = from the end).")},
              {"limit", integer("Max paragraphs (default 200).")}, {"runs", boolean("Include formatted runs with offsets.")},
              {"view", enm({"current", "original"}, "current = with changes applied (Word's 'Simple Markup'); original = before changes.")}},
             {"doc"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             assign_missing_para_ids(s.doc, s.doc.body(), s.doc.main());
             return read_paragraphs(s.doc, iarg(a, "offset", 1), iarg(a, "limit", 200), barg(a, "runs", false), sarg(a, "view", "current"));
         }});
    add({"get_text", "Plain text / Markdown", "The whole document as plain text or Markdown (headings, lists, tables).",
         obj({{"doc", kDoc}, {"format", enm({"text", "markdown"}, "Default markdown.")}, {"view", enm({"current", "original"}, "")}}, {"doc"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             return Json{{"text", to_text(s.doc, sarg(a, "view", "current"), sarg(a, "format", "markdown") == "markdown")}};
         }});
    add({"get_outline", "Outline", "Show the document outline: every heading with its level and paragraph id (the Navigation Pane). Use it to see the structure and jump to a section.", obj({{"doc", kDoc}}, {"doc"}), true, false,
         [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             assign_missing_para_ids(s.doc, s.doc.body(), s.doc.main());
             return outline(s.doc);
         }});
    add({"get_paragraph", "Paragraph details",
         "One paragraph: text, runs with formatting and offsets, direct and effective (style-resolved) formatting, list label.",
         obj({{"doc", kDoc}, {"paragraph", kPara}}, {"doc", "paragraph"}), true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             auto r = para(s, a);
             Json j = para_brief(s.doc, r.p);
             j["part"] = r.part;
             j["direct_format"] = ppr_to_json(r.p.child("w:pPr"));
             j["effective_format"] = effective_paragraph(s.doc, r.p);
             std::string lbl = list_label(s.doc, r.p);
             if (!lbl.empty()) j["list_label"] = lbl;
             auto rd = read_paragraphs(s.doc, 1, 0, true, "current");
             for (auto& pp : rd["paragraphs"]) if (pp["id"] == j["id"]) { j["runs"] = pp["runs"]; if (pp.contains("marks")) j["marks"] = pp["marks"]; }
             if (auto run = xml::first_descendant(r.p, "w:r")) j["effective_run_format"] = effective_run(s.doc, r.p, run);
             return j;
         }});
    add({"find_text", "Find",
         "Find text across the document (body, headers, footers, notes, comments). Returns paragraph ids and offsets for editing.",
         obj({{"doc", kDoc}, {"text", str("Text or regex to find.")}, {"match_case", boolean("Default true.")},
              {"whole_word", boolean("")}, {"regex", boolean("ECMAScript regex.")}, {"limit", integer("Max hits (default 100).")}},
             {"doc", "text"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             FindOpts f;
             f.match_case = barg(a, "match_case", true);
             f.whole_word = barg(a, "whole_word", false);
             f.regex = barg(a, "regex", false);
             f.max = iarg(a, "limit", 100);
             for (auto& part : s.doc.story_parts())
                 if (s.doc.pkg().has(part)) assign_missing_para_ids(s.doc, s.doc.pkg().xml(part).document_element(), part);
             Json v = Json::array();
             for (auto& h : find(s.doc, need(a, "text"), f)) {
                 auto t = analyze(h.p).text;
                 int cs = std::max(0, h.start - 30), ce = std::min((int)t.size(), h.end + 30);
                 Json j = {{"paragraph", (h.part == s.doc.main() ? "" : h.part + "#") + para_id(h.p)}, {"start", h.start}, {"end", h.end},
                           {"match", util::to_utf8(t.substr(h.start, h.end - h.start))},
                           {"context", util::to_utf8(t.substr(cs, ce - cs))}};
                 v.push_back(j);
             }
             return Json{{"count", v.size()}, {"hits", v}};
         }});
    add({"get_statistics", "Word count", "Word's Word Count: words, characters (with/without spaces), paragraphs, plus tables/comments/revisions/sections.",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) { return statistics(sess(S, a).doc); }});

    // ── text editing ─────────────────────────────────────────────────────
    add({"insert_text", "Insert text",
         "Type text into a paragraph at a position (default: end), or before/after given text. New text takes the formatting of "
         "the character before it, like typing in Word. \\t = tab, \\v = line break.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"text", str("Text to insert.")}, {"position", integer("Character offset (negative counts from end).")},
              {"after", str("Insert after this text.")}, {"before", str("Insert before this text.")}, {"occurrence", integer("Which occurrence of after/before (default 1).")},
              {"format", kRunFmt}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             int pos = pos_in(r.p, a);
             insert_text(s.doc, r.p, pos, util::to_u32(need(a, "text")), obj_arg(a, "format"), track(a));
             return para_brief(s.doc, r.p);
         }});
    add({"replace_text", "Replace text",
         "Replace text inside one paragraph (by match/occurrence or start/end). Tracked as deletion + insertion when tracking.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"match", str("Text to replace.")}, {"occurrence", integer("Default 1.")},
              {"start", integer("")}, {"end", integer("")}, {"text", str("Replacement text (empty deletes).")}, {"format", kRunFmt},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             auto [b, e] = range_in(r.p, a);
             replace_text(s.doc, r.p, b, e, util::to_u32(sarg(a, "text")), obj_arg(a, "format"), track(a));
             return para_brief(s.doc, r.p);
         }});
    add({"delete_text", "Delete text", "Delete text inside a paragraph (match/occurrence or start/end).",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"match", str("")}, {"occurrence", integer("")}, {"start", integer("")}, {"end", integer("")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph"}),
         false, true, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             auto [b, e] = range_in(r.p, a);
             delete_text(s.doc, r.p, b, e, track(a));
             return para_brief(s.doc, r.p);
         }});
    add({"find_replace", "Find and replace",
         "Replace All across the document (body, headers, footers, notes). Keeps the formatting of each match. "
         "Regex replacements support $1..$9.",
         obj({{"doc", kDoc}, {"find", str("Text or regex.")}, {"replace", str("Replacement.")}, {"match_case", boolean("Default true.")},
              {"whole_word", boolean("")}, {"regex", boolean("")}, {"format", kRunFmt}, {"limit", integer("Max replacements.")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "find", "replace"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             FindOpts f;
             f.match_case = barg(a, "match_case", true);
             f.whole_word = barg(a, "whole_word", false);
             f.regex = barg(a, "regex", false);
             f.max = iarg(a, "limit", -1);
             int n = replace_all(s.doc, need(a, "find"), sarg(a, "replace"), f, obj_arg(a, "format"), track(a));
             return Json{{"replaced", n}};
         }});
    add({"insert_paragraph", "Insert paragraph",
         "Insert new paragraph(s) before or after a paragraph (or a table: pass table). Lines in `text` become separate paragraphs. "
         "Inherits the neighbour's paragraph formatting unless `style` is given.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"table", integer("Anchor on this table (1-based) instead of a paragraph.")},
              {"text", str("Text; \\n separates paragraphs.")}, {"where", enm({"after", "before"}, "Default after.")},
              {"style", str("Paragraph style id or name, e.g. Heading1 / \"Heading 1\" / Quote.")}, {"format", kRunFmt},
              {"paragraph_format", kParaFmt}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             pugi::xml_node ref;
             if (a.contains("table")) ref = table_at(s.doc, iarg(a, "table", 1));
             else if (a.contains("paragraph")) ref = para(s, a).p;
             else ref = last_block(s.doc);
             // A blank document's only paragraph is where Word puts what you
             // type first; appending after it left an empty line at the top.
             bool fill_blank = !a.contains("paragraph") && !a.contains("table") && blocks(s.doc.body()).size() == 1 &&
                               std::string_view(ref.name()) == "w:p" && analyze(ref).atoms.empty() &&
                               !ref.child("w:pPr").child("w:sectPr") && !ref.child("w:pPr").child("w:numPr");
             auto np = insert_paragraph(s.doc, ref, sarg(a, "where", "after") == "after", util::to_u32(sarg(a, "text")),
                                        sarg(a, "style"), obj_arg(a, "format"), track(a));
             if (fill_blank) delete_paragraph(s.doc, ref, {false});
             Json made = Json::array();
             int lines = (int)util::split(sarg(a, "text"), '\n').size();
             auto cur = np;
             for (int i = 0; i < lines && cur; i++, cur = cur.next_sibling("w:p")) {
                 if (a.contains("paragraph_format")) format_paragraph(s.doc, cur, obj_arg(a, "paragraph_format"), track(a));
                 made.push_back(para_brief(s.doc, cur));
             }
             return Json{{"inserted", made}};
         }});
    add({"insert_markdown", "Insert Markdown",
         "Write Markdown as real Word content: # headings (Heading 1-6), paragraphs with **bold**, *italic*, `code`, ~~strike~~ and "
         "[links](url), - bullets and 1. numbered lists (indent 2 spaces to nest), > quotes, ``` code blocks, | tables |, --- rules, "
         "\\pagebreak. The fastest way to write a whole document. Goes after `paragraph` (or the end).",
         obj({{"doc", kDoc}, {"markdown", str("Markdown text.")}, {"paragraph", kPara},
              {"table", integer("Anchor on this table (1-based) instead of a paragraph.")},
              {"where", enm({"after", "before"}, "Default after.")}, {"track", kTrack}},
             {"doc", "markdown"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             pugi::xml_node ref;
             if (a.contains("table")) ref = table_at(s.doc, iarg(a, "table", 1));
             else if (a.contains("paragraph")) ref = para(s, a).p;
             else ref = last_block(s.doc);
             bool fill_blank = !a.contains("paragraph") && !a.contains("table") && blocks(s.doc.body()).size() == 1 &&
                               std::string_view(ref.name()) == "w:p" && analyze(ref).atoms.empty() &&
                               !ref.child("w:pPr").child("w:sectPr") && !ref.child("w:pPr").child("w:numPr");
             auto r = insert_markdown(s.doc, ref, sarg(a, "where", "after") == "after", need(a, "markdown"), track(a));
             if (fill_blank && (r.paragraphs.size() || r.tables)) delete_paragraph(s.doc, ref, {false});
             Json made = Json::array();
             for (std::size_t i = 0; i < r.paragraphs.size(); i++) {
                 if (i == 12 && r.paragraphs.size() > 14) {
                     made.push_back("... " + std::to_string(r.paragraphs.size() - 13) + " more");
                     i = r.paragraphs.size() - 1;
                 }
                 made.push_back(para_brief(s.doc, r.paragraphs[i]));
             }
             return Json{{"paragraphs", r.paragraphs.size()}, {"tables", r.tables}, {"lists", r.lists}, {"inserted", made}};
         }});
    add({"delete_paragraph", "Delete paragraph", "Delete whole paragraphs (text and paragraph mark).",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "Paragraph ids/numbers.")}, {"track", kTrack}, {"author", str("Author of this change.")}}, {"doc", "paragraphs"}),
         false, true, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::vector<pugi::xml_node> ps;
             for (auto& id : strs(a, "paragraphs")) ps.push_back(find_para(s.doc, id).p);
             for (auto p : ps) delete_paragraph(s.doc, p, track(a));
             return Json{{"deleted", ps.size()}};
         }});
    add({"split_paragraph", "Split paragraph (Enter)", "Press Enter at a position: splits the paragraph in two.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"position", integer("")}, {"after", str("Split after this text.")}, {"before", str("Split before this text.")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             auto np = split_paragraph(s.doc, r.p, pos_in(r.p, a), track(a));
             return Json{{"first", para_brief(s.doc, r.p)}, {"second", para_brief(s.doc, np)}};
         }});
    add({"merge_paragraphs", "Join with next paragraph", "Delete the paragraph mark, joining the paragraph with the next one.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"track", kTrack}, {"author", str("Author of this change.")}}, {"doc", "paragraph"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             merge_with_next(s.doc, r.p, track(a));
             return para_brief(s.doc, r.p);
         }});
    add({"move_paragraphs", "Move paragraphs",
         "Cut paragraphs and paste them elsewhere (before/after a target). With Track Changes this is a real Word "
         "move: shown as moved from / moved to, accepted or rejected as one.",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "Paragraph ids in order.")}, {"target", kPara}, {"where", enm({"after", "before"}, "")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraphs", "target"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::vector<pugi::xml_node> ps;
             for (auto& id : strs(a, "paragraphs")) ps.push_back(find_para(s.doc, id).p);
             auto tgt = find_para(s.doc, need(a, "target")).p;
             int n = move_paragraphs(s.doc, ps, tgt, sarg(a, "where", "after") == "after", track(a));
             return Json{{"moved", n}};
         }});

    // ── formatting ───────────────────────────────────────────────────────
    add({"format_text", "Format text (Font)", "Apply character formatting to text in a paragraph (match or start/end; default the whole paragraph).",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"match", str("")}, {"occurrence", integer("")}, {"start", integer("")}, {"end", integer("")},
              {"all_occurrences", boolean("Format every occurrence of match in the paragraph.")}, {"format", kRunFmt}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "format"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             if (barg(a, "all_occurrences", false) && a.contains("match")) {
                 auto t = analyze(r.p).text;
                 auto m = util::to_u32(sarg(a, "match"));
                 std::vector<std::pair<int, int>> rs;
                 for (std::size_t p = t.find(m); p != std::u32string::npos; p = t.find(m, p + m.size())) rs.push_back({(int)p, (int)(p + m.size())});
                 for (auto it = rs.rbegin(); it != rs.rend(); ++it) format_runs(s.doc, r.p, it->first, it->second, obj_arg(a, "format"), track(a));
                 return Json{{"formatted", rs.size()}, {"paragraph", para_brief(s.doc, r.p)}};
             }
             auto [b, e] = range_in(r.p, a);
             format_runs(s.doc, r.p, b, e, obj_arg(a, "format"), track(a));
             return Json{{"formatted", 1}, {"paragraph", para_brief(s.doc, r.p)}};
         }});
    add({"format_paragraph", "Format paragraph", "Apply paragraph formatting / style to one or more paragraphs.",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "Paragraph ids/numbers.")}, {"format", kParaFmt}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraphs", "format"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             int n = 0;
             for (auto& id : strs(a, "paragraphs")) { format_paragraph(s.doc, find_para(s.doc, id).p, obj_arg(a, "format"), track(a)); n++; }
             return Json{{"formatted", n}};
         }});
    add({"apply_style", "Apply style", "Apply a paragraph or character style by id or name (\"Heading 1\", \"Title\", \"Quote\", \"Strong\"...).",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "")}, {"style", str("")}, {"match", str("For a character style: text to style.")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraphs", "style"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::string sid = ensure_style(s.doc, need(a, "style"));
             auto st = find_style(s.doc, sid);
             bool character = xml::attr(st, "w:type") == "character";
             int n = 0;
             for (auto& id : strs(a, "paragraphs")) {
                 auto p = find_para(s.doc, id).p;
                 if (character) {
                     auto [b, e] = range_in(p, a);
                     format_runs(s.doc, p, b, e, {{"style", sid}}, track(a));
                 } else format_paragraph(s.doc, p, {{"style", sid}}, track(a));
                 n++;
             }
             return Json{{"style", sid}, {"applied_to", n}};
         }});
    add({"clear_formatting", "Clear formatting", "Remove direct character formatting (and optionally paragraph formatting), like Ctrl+Space / Ctrl+Q.",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "")}, {"paragraph_too", boolean("Also reset paragraph formatting to the style.")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraphs"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json clear = Json::object();
             for (auto k : {"bold", "italic", "underline", "strike", "double_strike", "caps", "small_caps", "font", "size", "color",
                            "highlight", "shading", "spacing", "position", "vertical_align", "hidden", "border", "kerning", "scale", "emphasis_mark"})
                 clear[k] = nullptr;
             int n = 0;
             for (auto& id : strs(a, "paragraphs")) {
                 auto p = find_para(s.doc, id).p;
                 int len = vlen(p);
                 if (len) format_runs(s.doc, p, 0, len, clear, track(a));
                 if (barg(a, "paragraph_too", false)) {
                     Json pc = Json::object();
                     for (auto k : {"alignment", "space_before", "space_after", "line_spacing", "indent_left", "indent_right",
                                    "first_line_indent", "hanging_indent", "shading", "borders", "tabs", "keep_with_next", "keep_together"})
                         pc[k] = nullptr;
                     format_paragraph(s.doc, p, pc, track(a));
                 }
                 n++;
             }
             return Json{{"cleared", n}};
         }});

    // ── styles ───────────────────────────────────────────────────────────
    add({"list_styles", "Styles", "Styles in the document; include_builtin adds Word's built-ins not yet used (added on first use).",
         obj({{"doc", kDoc}, {"include_builtin", boolean("")}, {"type", enm({"paragraph", "character", "table", "numbering"}, "Filter.")}}, {"doc"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             Json v = Json::array();
             for (auto& i : list_styles(s.doc, barg(a, "include_builtin", false))) {
                 if (a.contains("type") && i.type != a["type"].get<std::string>()) continue;
                 Json j = {{"id", i.id}, {"name", i.name}, {"type", i.type}};
                 if (!i.based_on.empty()) j["based_on"] = i.based_on;
                 if (i.in_use) j["in_use"] = true;
                 if (i.custom) j["custom"] = true;
                 if (i.quick) j["quick_style"] = true;
                 v.push_back(j);
             }
             return v;
         }});
    add({"get_style", "Style definition", "Show a style's settings: font, size, color, spacing, based on, next style. Works for Word's built-in styles too.", obj({{"doc", kDoc}, {"style", str("")}}, {"doc", "style"}), true, false,
         [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             auto st = find_style(s.doc, need(a, "style"));
             bool in_doc = (bool)st;
             if (!st) st = builtin_style(need(a, "style"));
             if (!st) throw std::runtime_error("style '" + sarg(a, "style") + "' not found; list_styles shows what exists");
             return Json{{"id", xml::attr(st, "w:styleId")}, {"name", xml::attr(st.child("w:name"), "w:val")}, {"type", xml::attr(st, "w:type")},
                         {"in_document", in_doc},
                         {"based_on", xml::attr(st.child("w:basedOn"), "w:val")}, {"next", xml::attr(st.child("w:next"), "w:val")},
                         {"paragraph", ppr_to_json(st.child("w:pPr"))}, {"run", rpr_to_json(st.child("w:rPr"))}};
         }});
    add({"modify_style", "Create / modify style",
         "Create a new style or modify one (Modify Style dialog). Changes apply everywhere the style is used.",
         obj({{"doc", kDoc}, {"style", str("Style id or name; a new id is derived from name when creating.")}, {"name", str("")},
              {"type", enm({"paragraph", "character", "table", "numbering"}, "For new styles.")}, {"based_on", str("")}, {"next", str("Style for the next paragraph.")},
              {"paragraph", kParaFmt}, {"run", kRunFmt}, {"quick_style", boolean("Show in the Styles gallery.")}, {"hidden", boolean("")}},
             {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json props = a;
             for (auto k : {"doc", "style", "author"}) props.erase(k);
             return Json{{"style", upsert_style(s.doc, sarg(a, "style"), props)}};
         }});
    add({"delete_style", "Delete style", "Delete a style; text using it reverts to Normal / Default Paragraph Font.",
         obj({{"doc", kDoc}, {"style", str("")}}, {"doc", "style"}), false, true, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             delete_style(s.doc, need(a, "style"));
             return Json{{"deleted", true}};
         }});

    // ── lists ────────────────────────────────────────────────────────────
    add({"apply_list", "Bullets / Numbering",
         "Make paragraphs a bulleted or numbered list (Word's default list definitions). Continues an adjacent list of the same kind.",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "")}, {"kind", enm({"bullet", "number", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman"}, "")},
              {"level", integer("0-8 (indent level).")}, {"start", integer("First number.")}, {"text", str("Number format like \"%1)\" or \"(%1)\".")},
              {"bullet", str("Bullet character.")}, {"continue_previous", boolean("Default true.")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraphs", "kind"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::vector<pugi::xml_node> ps;
             for (auto& id : strs(a, "paragraphs")) ps.push_back(find_para(s.doc, id).p);
             Json o = Json::object();
             for (auto k : {"start", "text", "bullet", "continue_previous"}) if (a.contains(k)) o[k] = a[k];
             int nid = apply_list(s.doc, ps, need(a, "kind"), iarg(a, "level", 0), o, track(a));
             Json labels = Json::array();
             for (auto p : ps) labels.push_back(list_label(s.doc, p));
             return Json{{"num_id", nid}, {"labels", labels}};
         }});
    add({"set_list_level", "Increase / decrease list level", "Indent or outdent list items: change the list level (Tab / Shift+Tab in a bulleted or numbered list).",
         obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "")}, {"level", integer("0-8")}, {"track", kTrack}}, {"doc", "paragraphs", "level"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json out = Json::array();
             for (auto& id : strs(a, "paragraphs")) {
                 auto p = find_para(s.doc, id).p;
                 auto np = p.child("w:pPr").child("w:numPr");
                 if (!np) throw std::runtime_error("paragraph " + id + " is not in a list");
                 format_paragraph(s.doc, p, {{"numbering", {{"num_id", util::to_int(xml::attr(np.child("w:numId"), "w:val"))}, {"level", iarg(a, "level", 0)}}}}, track(a));
                 Json j = para_brief(s.doc, p);
                 j["label"] = list_label(s.doc, p);
                 out.push_back(j);
             }
             return Json{{"level", iarg(a, "level", 0)}, {"paragraphs", out}};
         }});
    add({"remove_list", "Remove bullets / numbering", "Remove bullets or numbering from paragraphs, keeping the text.", obj({{"doc", kDoc}, {"paragraphs", arr(str(""), "")}, {"track", kTrack}}, {"doc", "paragraphs"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::vector<pugi::xml_node> ps;
             for (auto& id : strs(a, "paragraphs")) ps.push_back(find_para(s.doc, id).p);
             remove_list(s.doc, ps, track(a));
             Json out = Json::array();
             for (auto p : ps) out.push_back(para_brief(s.doc, p));
             return Json{{"paragraphs", out}};
         }});
    add({"restart_numbering", "Restart numbering", "Restart a numbered list at a paragraph, from 1 or any number (Set Numbering Value).",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"start", integer("Default 1.")}}, {"doc", "paragraph"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             restart_numbering(s.doc, r.p, iarg(a, "start", 1));
             return Json{{"label", list_label(s.doc, r.p)}};
         }});

    // ── track changes ────────────────────────────────────────────────────
    add({"set_track_changes", "Track Changes on/off", "Turn Track Changes on or off for the document (Review > Track Changes).",
         obj({{"doc", kDoc}, {"on", boolean("")}, {"author", str("Author name for subsequent changes.")}}, {"doc", "on"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             s.doc.set_track_changes(barg(a, "on", true));
             return Json{{"track_changes", s.doc.track_changes()}, {"author", s.doc.author.name}};
         }});
    add({"list_revisions", "Revisions", "Every tracked change: id, type, author, date, text, paragraph.",
         obj({{"doc", kDoc}, {"author", str("Filter by author.")}, {"type", str("Filter by type (insertion, deletion, run_format, ...).")}}, {"doc"}), true, false,
         [](Sessions& S, const Json& a) {
             auto& s = S.get(need(a, "doc"));
             Json v = Json::array();
             for (auto& r : list_revisions(s.doc)) {
                 if (a.contains("author") && r.author != sarg(a, "author")) continue;
                 if (a.contains("type") && r.type != a["type"].get<std::string>()) continue;
                 Json j = {{"id", r.id}, {"type", r.type}, {"author", r.author}, {"date", r.date}};
                 if (!r.text.empty()) j["text"] = r.text;
                 if (!r.para.empty()) j["paragraph"] = r.para;
                 if (r.part != s.doc.main()) j["part"] = r.part;
                 v.push_back(j);
             }
             return Json{{"count", v.size()}, {"revisions", v}};
         }});
    add({"accept_revisions", "Accept changes", "Accept tracked changes: all, by ids, or by author.",
         obj({{"doc", kDoc}, {"ids", arr(str(""), "Revision ids (from list_revisions). Omit for all.")}, {"by_author", str("Only this author's changes.")}}, {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             RevFilter f;
             f.ids = strs(a, "ids");
             f.author = sarg(a, "by_author");
             return Json{{"accepted", accept_revisions(s.doc, f)}, {"remaining", list_revisions(s.doc).size()}};
         }});
    add({"reject_revisions", "Reject changes", "Reject tracked changes: all, by ids, or by author.",
         obj({{"doc", kDoc}, {"ids", arr(str(""), "")}, {"by_author", str("")}}, {"doc"}), false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             RevFilter f;
             f.ids = strs(a, "ids");
             f.author = sarg(a, "by_author");
             return Json{{"rejected", reject_revisions(s.doc, f)}, {"remaining", list_revisions(s.doc).size()}};
         }});
    add({"compare_documents", "Compare",
         "Review > Compare: build a new document showing `revised` as tracked changes against `original`, saved to `output`.",
         obj({{"original", str("Path of the original .docx.")}, {"revised", str("Path of the revised .docx.")}, {"output", str("Where to write the comparison.")},
              {"author", str("Author shown for the changes.")}},
             {"original", "revised", "output"}),
         false, false, [](Sessions& S, const Json& a) {
             auto o = Document::open(need(a, "original")), r = Document::open(need(a, "revised"));
             accept_revisions(o, {});
             accept_revisions(r, {});
             auto c = compare(o, r, sarg(a, "author", "Compare"), {});
             c.save(need(a, "output"));
             std::string h = S.open(need(a, "output"), false);
             return Json{{"doc", h}, {"revisions", list_revisions(S.get(h).doc).size()}};
         }});

    // ── comments ─────────────────────────────────────────────────────────
    add({"add_comment", "New comment", "Comment on text in a paragraph (match or start/end; default the whole paragraph).",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"match", str("")}, {"occurrence", integer("")}, {"start", integer("")}, {"end", integer("")},
              {"end_paragraph", str("For a range spanning paragraphs: the paragraph where it ends (end = offset there).")},
              {"text", str("Comment text (\\n for new paragraphs).")}, {"author", str("Author of this change.")}, {"initials", str("")}},
             {"doc", "paragraph", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             if (a.contains("initials")) s.doc.author.initials = a["initials"].get<std::string>();
             auto r = para(s, a);
             std::string id;
             if (a.contains("end_paragraph")) {
                 auto r2 = find_para(s.doc, need(a, "end_paragraph"));
                 id = add_comment_range(s.doc, r.p, iarg(a, "start", 0), r2.p, iarg(a, "end", vlen(r2.p)), need(a, "text"));
             } else {
                 auto [b, e] = range_in(r.p, a);
                 id = add_comment(s.doc, r.p, b, e, need(a, "text"));
             }
             return Json{{"comment_id", id}};
         }});
    add({"list_comments", "Comments", "All comments with author, date, text, the commented text, thread parent and resolved state.",
         obj({{"doc", kDoc}, {"include_resolved", boolean("Default true.")}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
             auto& s = S.get(need(a, "doc"));
             Json v = Json::array();
             for (auto& c : list_comments(s.doc)) {
                 if (!barg(a, "include_resolved", true) && c.done) continue;
                 Json j = {{"id", c.id}, {"author", c.author}, {"date", c.date}, {"text", c.text}, {"resolved", c.done}};
                 if (!c.parent_id.empty()) j["reply_to"] = c.parent_id;
                 if (!c.anchor_text.empty()) j["on_text"] = c.anchor_text;
                 if (!c.anchor_para.empty()) j["paragraph"] = c.anchor_para;
                 v.push_back(j);
             }
             return v;
         }});
    add({"reply_comment", "Reply", "Reply to a comment (threaded like Word's Reply).",
         obj({{"doc", kDoc}, {"comment_id", str("")}, {"text", str("")}, {"author", str("Author of this change.")}}, {"doc", "comment_id", "text"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             return Json{{"comment_id", reply_comment(s.doc, need(a, "comment_id"), need(a, "text"))}};
         }});
    add({"resolve_comment", "Resolve / reopen", "Mark a comment thread resolved (or reopen it).",
         obj({{"doc", kDoc}, {"comment_id", str("")}, {"resolved", boolean("Default true.")}}, {"doc", "comment_id"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             resolve_comment(s.doc, need(a, "comment_id"), barg(a, "resolved", true));
             return Json{{"comment_id", need(a, "comment_id")}, {"resolved", barg(a, "resolved", true)}};
         }});
    add({"edit_comment", "Edit comment", "Change a comment's text.", obj({{"doc", kDoc}, {"comment_id", str("")}, {"text", str("")}}, {"doc", "comment_id", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             edit_comment(s.doc, need(a, "comment_id"), need(a, "text"));
             return Json{{"comment_id", need(a, "comment_id")}, {"text", need(a, "text")}};
         }});
    add({"delete_comment", "Delete comment", "Delete a comment and its replies; or all comments (optionally by author).",
         obj({{"doc", kDoc}, {"comment_id", str("Omit with all=true to delete every comment.")}, {"all", boolean("")}, {"by_author", str("")}}, {"doc"}),
         false, true, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             if (barg(a, "all", false)) return Json{{"deleted", delete_all_comments(s.doc, sarg(a, "by_author"))}};
             delete_comment(s.doc, need(a, "comment_id"));
             return Json{{"deleted", 1}};
         }});

    // ── tables ───────────────────────────────────────────────────────────
    add({"insert_table", "Insert table",
         "Insert a table after/before a paragraph. rows = array of arrays of cell text. Default style Table Grid, autofit to window.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"where", enm({"after", "before"}, "")},
              {"rows", Json{{"type", "array"}, {"description", "Rows: [[\"Item\",\"Price\"],[\"Setup\",\"500\"]], or objects [{\"Item\":\"Setup\",\"Price\":500}] (keys become the header row)."}}}, {"style", str("Table style, e.g. TableGrid, \"Grid Table 4 Accent 1\".")},
              {"header_row", boolean("Default true.")}, {"repeat_header", boolean("Repeat header row on each page.")},
              {"widths", arr(num(""), "Column widths (pt or \"1.5in\").")}, {"alignment", enm({"left", "center", "right"}, "")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "rows"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             pugi::xml_node ref = a.contains("paragraph") ? para(s, a).p : last_block(s.doc);
             std::vector<std::vector<std::string>> rows;
             auto cell_text = [](const Json& c) { return c.is_string() ? c.get<std::string>() : c.is_null() ? std::string() : c.dump(); };
             Json in = a["rows"];
             if (in.is_array() && !in.empty() && in[0].is_object()) {
                 // [{"Item":"Setup","Price":500}, ...] -> header row from the keys
                 std::vector<std::string> keys;
                 for (auto& [k, _] : in[0].items()) keys.push_back(k);
                 rows.push_back(keys);
                 for (auto& r : in) {
                     std::vector<std::string> row;
                     for (auto& k : keys) row.push_back(r.contains(k) ? cell_text(r[k]) : "");
                     rows.push_back(row);
                 }
             } else {
                 for (auto& r : in) {
                     std::vector<std::string> row;
                     if (r.is_array()) for (auto& c : r) row.push_back(cell_text(c));
                     else if (r.is_string()) {
                         // one row given as "a | b | c" or tab separated
                         std::string sr = r.get<std::string>();
                         char sep = sr.find('\t') != std::string::npos ? '\t' : '|';
                         for (auto& c : util::split(sr, sep)) row.push_back(util::trim(c));
                     } else row.push_back(cell_text(r));
                     rows.push_back(row);
                 }
             }
             Json o = Json::object();
             for (auto k : {"style", "header_row", "repeat_header", "widths", "alignment"}) if (a.contains(k)) o[k] = a[k];
             auto tbl = insert_table(s.doc, ref, sarg(a, "where", "after") == "after", rows, o, track(a));
             auto ts = tables(s.doc);
             int idx = int(std::find(ts.begin(), ts.end(), tbl) - ts.begin()) + 1;
             return Json{{"table", idx}, {"rows", rows.size()}};
         }});
    add({"read_table", "Read table", "A table's cells (text), spans and style. Tables are numbered 1.. in document order.",
         obj({{"doc", kDoc}, {"table", integer("1-based")}}, {"doc", "table"}), true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             return table_json(table_at(s.doc, iarg(a, "table", 1)));
         }});
    add({"set_cell", "Set cell text / format", "Replace a cell's text and/or format it. row/col are 1-based.",
         obj({{"doc", kDoc}, {"table", integer("")}, {"row", integer("")}, {"col", integer("")}, {"text", str("")},
              {"format", anyobj("Cell: shading, vertical_align (top|center|bottom), width, borders, text_direction, no_wrap, paragraph{..}, run{..}")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "table", "row", "col"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tc = cell(table_at(s.doc, iarg(a, "table", 1)), iarg(a, "row", 1) - 1, iarg(a, "col", 1) - 1);
             if (a.contains("text")) set_cell_text(s.doc, tc, sarg(a, "text"), track(a));
             if (a.contains("format")) format_cell(s.doc, tc, obj_arg(a, "format"), track(a));
             std::string text;
             for (auto p : paragraphs(tc)) text += (text.empty() ? "" : "\n") + analyze(p).utf8();
             return Json{{"table", iarg(a, "table", 1)}, {"row", iarg(a, "row", 1)}, {"col", iarg(a, "col", 1)}, {"text", text}};
         }});
    add({"table_rows", "Insert / delete rows", "Add a row above or below, or delete rows, in a table. row is 1-based.",
         obj({{"doc", kDoc}, {"table", integer("")}, {"action", enm({"insert_above", "insert_below", "delete"}, "")}, {"row", integer("")},
              {"cells", arr(str(""), "Texts for the new row.")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "table", "action", "row"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             std::string act = need(a, "action");
             int r = iarg(a, "row", 1) - 1;
             if (act == "delete") delete_row(s.doc, tbl, r, track(a));
             else insert_row(s.doc, tbl, r, act == "insert_below", strs(a, "cells"), track(a));
             return table_json(tbl);
         }});
    add({"table_columns", "Insert / delete columns", "Add or delete table columns (insert_left, insert_right, delete). col is 1-based.", obj({{"doc", kDoc}, {"table", integer("")}, {"action", enm({"insert_left", "insert_right", "delete"}, "")},
                                                         {"col", integer("")}, {"track", kTrack}, {"author", str("Author of this change.")}},
                                                        {"doc", "table", "action", "col"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             std::string act = need(a, "action");
             int c = iarg(a, "col", 1) - 1;
             if (act == "delete") delete_column(s.doc, tbl, c, track(a));
             else insert_column(s.doc, tbl, c, act == "insert_right", track(a));
             return table_json(tbl);
         }});
    add({"merge_cells", "Merge cells", "Merge table cells: a rectangle of cells becomes one (1-based, inclusive).",
         obj({{"doc", kDoc}, {"table", integer("")}, {"from_row", integer("")}, {"from_col", integer("")}, {"to_row", integer("")}, {"to_col", integer("")}},
             {"doc", "table", "from_row", "from_col", "to_row", "to_col"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             merge_cells(s.doc, tbl, iarg(a, "from_row", 1) - 1, iarg(a, "from_col", 1) - 1, iarg(a, "to_row", 1) - 1, iarg(a, "to_col", 1) - 1);
             return table_json(tbl);
         }});
    add({"split_cell", "Split cells",
         "Split table cells: undo a merge at a cell, or split a plain cell into several side by side (cols). "
         "row/col are 1-based.",
         obj({{"doc", kDoc}, {"table", integer("")}, {"row", integer("")}, {"col", integer("")},
              {"cols", integer("For a plain cell: how many cells to split it into (default 2).")}},
             {"doc", "table", "row", "col"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             int n = split_cell(s.doc, tbl, iarg(a, "row", 1) - 1, iarg(a, "col", 1) - 1, iarg(a, "cols", 2));
             Json out = table_json(tbl);
             out["cells"] = n;
             return out;
         }});
    add({"format_table", "Table properties",
         "style, alignment, width (pt|\"50%\"|auto), borders (\"all\"|\"none\"|{top,insideH,..}), indent, layout (fixed|autofit), "
         "cell_margins, header_row, first_column, last_row, last_column, banded_rows, banded_columns, repeat_header, row_height, column_widths.",
         obj({{"doc", kDoc}, {"table", integer("")}, {"format", anyobj("")}, {"track", kTrack}}, {"doc", "table", "format"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             format_table(s.doc, tbl, obj_arg(a, "format"), track(a));
             Json out = {{"table", iarg(a, "table", 1)}, {"applied", Json::array()}};
             for (auto& [k, _] : obj_arg(a, "format").items()) out["applied"].push_back(k);
             std::string sty = xml::attr(tbl.child("w:tblPr").child("w:tblStyle"), "w:val");
             if (!sty.empty()) out["style"] = sty;
             return out;
         }});
    add({"sort_table", "Sort table",
         "Sort a table's rows by a column (Table > Sort). The header row stays on top. type: text, number or date.",
         obj({{"doc", kDoc}, {"table", integer("")}, {"col", integer("Column to sort by (1-based).")},
              {"descending", boolean("")}, {"type", enm({"text", "number", "date"}, "Default text.")},
              {"header", boolean("First row is a header (default true).")}},
             {"doc", "table", "col"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto tbl = table_at(s.doc, iarg(a, "table", 1));
             int n = sort_table(s.doc, tbl, iarg(a, "col", 1) - 1, barg(a, "descending", false), sarg(a, "type", "text"),
                                barg(a, "header", true), {false});
             Json col = Json::array();
             for (auto& r : table_json(tbl)["rows"]) col.push_back(r.size() >= (std::size_t)iarg(a, "col", 1) ? r[iarg(a, "col", 1) - 1] : Json(""));
             return Json{{"sorted_rows", n}, {"column_now", col}};
         }});
    add({"convert_table", "Convert text / table",
         "Convert Text to Table (paragraphs -> rows, cells split on separator) or Table to Text (rows -> paragraphs).",
         obj({{"doc", kDoc}, {"to", enm({"table", "text"}, "")}, {"paragraphs", arr(str(""), "to table: the paragraphs, in order.")},
              {"table", integer("to text: the table.")}, {"separator", str("tab (default), comma, semicolon or any text.")},
              {"style", str("to table: table style, e.g. Grid Table 4 Accent 1.")}, {"header_row", boolean("to table: first row is a header.")},
              {"track", kTrack}},
             {"doc", "to"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::string sep = sarg(a, "separator", "tab");
             if (need(a, "to") == "table") {
                 std::vector<pugi::xml_node> ps;
                 for (auto& id : strs(a, "paragraphs")) ps.push_back(find_para(s.doc, id).p);
                 Json opts = {{"header_row", barg(a, "header_row", true)}};
                 if (a.contains("style")) opts["style"] = sarg(a, "style");
                 auto tbl = text_to_table(s.doc, ps, sep, opts, track(a));
                 int n = 0, i = 0;
                 for (auto t : tables(s.doc)) { i++; if (t == tbl) n = i; }
                 return Json{{"table", n}, {"rows", xml::children(tbl, "w:tr").size()},
                             {"columns", xml::children(tbl.child("w:tblGrid"), "w:gridCol").size()}};
             }
             if (need(a, "to") != "text") throw std::runtime_error("to must be table or text");
             auto ps = table_to_text(s.doc, table_at(s.doc, iarg(a, "table", 1)), sep, track(a));
             Json out = Json::array();
             for (auto p : ps) out.push_back(para_brief(s.doc, p));
             return Json{{"paragraphs", out}};
         }});
    add({"delete_table", "Delete table", "Delete a whole table from the document.", obj({{"doc", kDoc}, {"table", integer("")}, {"track", kTrack}, {"author", str("Author of this change.")}}, {"doc", "table"}), false, true,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             delete_table(s.doc, table_at(s.doc, iarg(a, "table", 1)), track(a));
             return Json{{"deleted", iarg(a, "table", 1)}, {"tables", tables(s.doc).size()}};
         }});

    // ── insert: pictures, breaks, links, notes, fields, TOC, equations ──
    const Json kLayout = {
        {"description",
         "Layout Options. \"inline\" (default) flows with the text; \"square\", \"tight\", \"through\", \"top_bottom\" wrap "
         "text around it; \"behind\" / \"in_front\" float over the text. Or an object: {wrap, horizontal: left|center|right, "
         "vertical: top|center|bottom, x, y (points or \"1in\"), horizontal_from: margin|page|column|character, "
         "vertical_from: margin|page|paragraph|line, distance (gap to text, points)}."}};
    add({"insert_picture", "Insert picture",
         "Insert a picture or image (photo, logo, chart image) from a file (PNG, JPEG, GIF, BMP, TIFF, SVG, EMF, WMF) or "
         "base64 data. Size in points; give width or height to keep the aspect ratio. Too-wide pictures are scaled to "
         "the text width like Word. layout makes it float with text wrapping.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"position", integer("")}, {"after", str("")}, {"file", str("Image file path.")},
              {"data_base64", str("Image bytes (alternative to file).")}, {"width", num("pt")}, {"height", num("pt")},
              {"alt_text", str("Alt text for accessibility.")}, {"layout", kLayout}, {"track", kTrack},
              {"author", str("Author of this change.")}},
             {"doc", "paragraph"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             util::Bytes data;
             if (a.contains("file")) data = load_file(sarg(a, "file"));
             else if (a.contains("data_base64")) {
                 auto b = util::base64_decode(a["data_base64"].get<std::string>());
                 if (!b) throw std::runtime_error("bad base64");
                 data = *b;
             } else throw std::runtime_error("give file or data_base64");
             auto place = placement_from_json(a.contains("layout") ? a["layout"] : Json());
             insert_picture(s.doc, r.p, pos_in(r.p, a), data, darg(a, "width", 0.0), darg(a, "height", 0.0), sarg(a, "alt_text"),
                            r.part, track(a), place);
             auto all = list_pictures(s.doc);
             int n = 0;
             for (auto& pi : all)
                 if (pi.para == para_id(r.p)) n = pi.index;
             return Json{{"picture", n}, {"layout", place.wrap}, {"paragraph", para_id(r.p)}};
         }});
    add({"insert_text_box", "Insert text box",
         "Insert a text box (Insert > Text Box): a bordered box of text that floats with text wrapped around it, for "
         "callouts, sidebars and pull quotes. Lines in `text` become paragraphs; edit them later by the returned ids.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"position", integer("")}, {"text", str("Text; \\n separates paragraphs.")},
              {"width", num("pt, default 216 (3in)")}, {"height", num("pt, default fits the text")},
              {"fill", str("Background, RRGGBB or a name.")},
              {"border", str("Outline color, or false for none. Object {color, size} also works.")},
              {"shape", enm({"rect", "rounded"}, "Default rect.")}, {"alignment", enm({"left", "center", "right"}, "")},
              {"format", kRunFmt}, {"layout", kLayout}, {"track", kTrack}},
             {"doc", "paragraph", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             Json style = Json::object();
             for (auto k : {"fill", "shape", "alignment"}) if (a.contains(k)) style[k] = a[k];
             if (a.contains("border")) {
                 const Json& b = a["border"];
                 if (b.is_string() && (b == "false" || b == "none")) style["border"] = false;
                 else style["border"] = b;
             }
             if (a.contains("format")) style["format"] = obj_arg(a, "format");
             Json lay = a.contains("layout") ? a["layout"] : Json("square");
             auto place = placement_from_json(lay);
             auto ps = insert_text_box(s.doc, r.p, pos_in(r.p, a), need(a, "text"), darg(a, "width", 0.0), darg(a, "height", 0.0),
                                       style, track(a), place);
             Json out = Json::array();
             for (auto p : ps) out.push_back(para_brief(s.doc, p));
             return Json{{"text_box", out}, {"layout", place.wrap}, {"anchored_in", para_id(r.p)}};
         }});
    add({"list_pictures", "Pictures",
         "List the pictures in the document: number, size in points, layout (inline or how text wraps), alt text and "
         "the paragraph each sits in.",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
             Json v = Json::array();
             for (auto& p : list_pictures(sess(S, a).doc))
                 v.push_back({{"picture", p.index}, {"name", p.name}, {"alt_text", p.alt}, {"paragraph", p.para},
                              {"layout", p.wrap}, {"width", std::round(p.width_pt * 10) / 10},
                              {"height", std::round(p.height_pt * 10) / 10}});
             return v;
         }});
    add({"format_picture", "Resize / position picture",
         "Resize a picture, change how text wraps around it (Layout Options), move it, or set its alt text. Give width "
         "or height alone to keep the aspect ratio.",
         obj({{"doc", kDoc}, {"picture", integer("From list_pictures (1-based).")}, {"width", num("pt")}, {"height", num("pt")},
              {"layout", kLayout}, {"alt_text", str("")}},
             {"doc", "picture"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             int n = iarg(a, "picture", 1);
             set_picture(s.doc, n, darg(a, "width", 0.0), darg(a, "height", 0.0), a.contains("layout") ? a["layout"] : Json(),
                         a.contains("alt_text") ? a["alt_text"] : Json());
             auto p = list_pictures(s.doc)[n - 1];
             return Json{{"picture", n}, {"layout", p.wrap}, {"width", std::round(p.width_pt * 10) / 10},
                         {"height", std::round(p.height_pt * 10) / 10}};
         }});
    add({"delete_picture", "Delete picture", "Delete a picture (number from list_pictures).",
         obj({{"doc", kDoc}, {"picture", integer("")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "picture"}),
         false, true, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             delete_picture(s.doc, iarg(a, "picture", 1), track(a));
             return Json{{"ok", true}, {"remaining", list_pictures(s.doc).size()}};
         }});
    add({"insert_break", "Insert break", "Page break, column break, line break, or a section break after a paragraph.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"type", enm({"page", "column", "line", "section_next_page", "section_continuous", "section_even_page", "section_odd_page"}, "")},
              {"position", integer("For page/column/line breaks.")}, {"after", str("")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "type"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             std::string t = need(a, "type");
             if (util::starts_with(t, "section_")) {
                 static const std::map<std::string, std::string> m = {{"section_next_page", "nextPage"}, {"section_continuous", "continuous"},
                                                                      {"section_even_page", "evenPage"}, {"section_odd_page", "oddPage"}};
                 insert_section_break(s.doc, r.p, m.at(t), track(a));
                 return Json{{"sections", sections(s.doc).size()}};
             }
             insert_break(s.doc, r.p, pos_in(r.p, a), t, track(a));
             return Json{{"type", t}, {"paragraph", para_brief(s.doc, r.p)}};
         }});
    add({"insert_hyperlink", "Insert link", "Link text to a URL or to a bookmark (\"#name\"). Wrap existing text (match) or insert new link text.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"url", str("https://..., mailto:..., or #bookmark")}, {"match", str("Existing text to link.")},
              {"occurrence", integer("")}, {"text", str("New link text to insert.")}, {"position", integer("Where to insert new text.")},
              {"tooltip", str("")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "url"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             int b, e;
             if (a.contains("match")) std::tie(b, e) = range_in(r.p, a);
             else { b = e = pos_in(r.p, a); }
             add_hyperlink(s.doc, r.p, b, e, need(a, "url"), sarg(a, "text"), sarg(a, "tooltip"), track(a));
             return para_brief(s.doc, r.p);
         }});
    add({"list_hyperlinks", "Links", "List the links in the document: their text, URL or bookmark target, and paragraph.", obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
        Json v = Json::array();
        for (auto& l : list_hyperlinks(sess(S, a).doc)) v.push_back({{"text", l.text}, {"target", l.target}, {"paragraph", l.para}});
        return v;
    }});
    add({"remove_hyperlinks", "Remove links", "Remove hyperlinks (keeping the text). Filter by target substring.",
         obj({{"doc", kDoc}, {"target_contains", str("")}}, {"doc"}), false, false, [](Sessions& S, const Json& a) {
             return Json{{"removed", remove_hyperlinks(mut(S, a).doc, sarg(a, "target_contains"))}};
         }});
    add({"add_bookmark", "Bookmark", "Add a bookmark on text (match/start/end) or at a point, for links and cross-references.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"name", str("Letters, digits, _; starts with a letter; max 40.")}, {"match", str("")},
              {"start", integer("")}, {"end", integer("")}},
             {"doc", "paragraph", "name"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             auto [b, e] = a.contains("match") || a.contains("start") ? range_in(r.p, a) : std::pair<int, int>{0, vlen(r.p)};
             add_bookmark(s.doc, r.p, b, r.p, e, need(a, "name"));
             auto pt = analyze(r.p);
             std::u32string sub = pt.text.substr(std::min<size_t>(b, pt.text.size()), std::max(0, e - b));
             return Json{{"name", need(a, "name")}, {"paragraph", para_id(r.p)}, {"text", util::to_utf8(sub)}};
         }});
    add({"list_bookmarks", "Bookmarks", "List bookmarks with the text they mark. include_hidden adds Word's hidden _Toc/_Ref ones.", obj({{"doc", kDoc}, {"include_hidden", boolean("Include _Toc/_Ref/_GoBack bookmarks.")}}, {"doc"}), true, false,
         [](Sessions& S, const Json& a) {
             Json v = Json::array();
             for (auto& b : list_bookmarks(sess(S, a).doc)) {
                 if (!barg(a, "include_hidden", false) && !b.name.empty() && b.name[0] == '_') continue;
                 v.push_back({{"name", b.name}, {"text", b.text}, {"paragraph", b.para}});
             }
             return v;
         }});
    add({"delete_bookmark", "Delete bookmark", "Remove a bookmark (the text stays).", obj({{"doc", kDoc}, {"name", str("")}}, {"doc", "name"}), false, false, [](Sessions& S, const Json& a) {
        delete_bookmark(mut(S, a).doc, need(a, "name"));
        return Json{{"deleted", need(a, "name")}};
    }});
    add({"insert_note", "Footnote / endnote", "Insert a footnote or endnote reference at a position with the note text.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"kind", enm({"footnote", "endnote"}, "")}, {"text", str("")}, {"position", integer("")},
              {"after", str("Place the reference mark after this text.")}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             return Json{{"note_id", add_note(s.doc, r.p, pos_in(r.p, a), sarg(a, "kind", "footnote"), need(a, "text"), track(a))}};
         }});
    add({"list_notes", "Footnotes / endnotes", "List footnotes and endnotes with their text and the paragraph they belong to.", obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
        Json v = Json::array();
        for (auto& n : list_notes(sess(S, a).doc)) v.push_back({{"id", n.id}, {"kind", n.kind}, {"text", n.text}, {"paragraph", n.anchor_para}});
        return v;
    }});
    add({"delete_note", "Delete note", "Delete a footnote or endnote and its reference mark.", obj({{"doc", kDoc}, {"kind", enm({"footnote", "endnote"}, "")}, {"id", str("")}, {"track", kTrack}}, {"doc", "kind", "id"}),
         false, true, [](Sessions& S, const Json& a) {
             delete_note(mut(S, a).doc, need(a, "kind"), need(a, "id"), track(a));
             return Json{{"deleted", need(a, "kind") + " " + need(a, "id")}};
         }});
    add({"insert_field", "Insert field",
         "Insert a Word field: PAGE, NUMPAGES, DATE \\@ \"MMMM d, yyyy\", TIME, AUTHOR, TITLE, FILENAME, REF bookmark \\h, "
         "PAGEREF bookmark \\h, SEQ Figure \\* ARABIC, STYLEREF \"Heading 1\", NUMWORDS, MERGEFIELD Name, IF ...",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"code", str("Field instruction.")}, {"result", str("Cached display text (Word updates it).")},
              {"position", integer("")}, {"after", str("")}, {"format", kRunFmt}, {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "code"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             insert_field(s.doc, r.p, pos_in(r.p, a), need(a, "code"), sarg(a, "result"), obj_arg(a, "format"), track(a));
             return Json{{"code", need(a, "code")}, {"paragraph", para_brief(s.doc, r.p)}};
         }});
    add({"list_fields", "Fields", "List fields (PAGE, DATE, TOC, REF, MERGEFIELD...) with their codes and current results.", obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
        Json v = Json::array();
        for (auto& f : list_fields(sess(S, a).doc)) v.push_back({{"code", f.instr}, {"result", f.result}, {"paragraph", f.para}});
        return v;
    }});
    add({"update_fields", "Update fields",
         "Update TOC entries, SEQ numbering and REF results now; page numbers are refreshed by Word on open (updateFields is set).",
         obj({{"doc", kDoc}}, {"doc"}), false, false, [](Sessions& S, const Json& a) { return Json{{"updated", update_fields(mut(S, a).doc)}}; }});
    add({"insert_toc", "Table of contents",
         "Insert a table of contents (References > Table of Contents) before a paragraph, built from headings. "
         "captions: \"Figure\" (or Table, ...) makes a Table of Figures from those captions instead.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"from_level", integer("Default 1.")}, {"to_level", integer("Default 3.")},
              {"captions", str("Caption label for a Table of Figures, e.g. Figure or Table.")},
              {"title", str("Heading above the TOC (default \"Contents\"; empty for none).")}, {"hyperlinks", boolean("Default true.")}, {"track", kTrack}},
             {"doc", "paragraph"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json o = Json::object();
             for (auto k : {"from_level", "to_level", "title", "hyperlinks", "captions"}) if (a.contains(k)) o[k] = a[k];
             insert_toc(s.doc, para(s, a).p, o, track(a));
             return Json{{"inserted", a.contains("captions") ? "table of figures (" + sarg(a, "captions") + ")" : "table of contents"},
                         {"note", "page numbers fill in when the document is opened in Word or rendered (render_pages, export_pdf)"}};
         }});
    add({"mark_index_entry", "Mark index entry",
         "Mark a word or phrase for the index (References > Mark Entry). Put it after `match` text (or at position). "
         "sub makes a sub-entry under term. Then insert_index builds the index.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"term", str("Index entry text.")}, {"sub", str("Sub-entry under term.")},
              {"match", str("Mark this text (the entry goes right after it; default term).")}, {"position", integer("")},
              {"all", boolean("Mark every paragraph that contains the text (Mark All).")}, {"track", kTrack}},
             {"doc", "term"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             std::string term = need(a, "term"), sub = sarg(a, "sub");
             std::string m = sarg(a, "match", term);
             auto mark_in = [&](pugi::xml_node p) -> bool {
                 auto text = analyze(p).text;
                 auto u = util::to_u32(m);
                 auto at = text.find(u);
                 if (at == std::u32string::npos) return false;
                 mark_index_entry(s.doc, p, (int)(at + u.size()), term, sub, track(a));
                 return true;
             };
             Json marked = Json::array();
             if (barg(a, "all", false)) {
                 for (auto p : paragraphs(s.doc.body(), false))
                     if (mark_in(p)) marked.push_back(para_id(p));
             } else {
                 if (!a.contains("paragraph")) throw std::runtime_error("give paragraph, or all: true to mark every occurrence");
                 auto r = para(s, a);
                 if (a.contains("position")) mark_index_entry(s.doc, r.p, pos_in(r.p, a), term, sub, track(a));
                 else if (!mark_in(r.p)) throw std::runtime_error("'" + m + "' is not in paragraph " + para_id(r.p));
                 marked.push_back(para_id(r.p));
             }
             if (marked.empty()) throw std::runtime_error("'" + m + "' was not found in the document");
             return Json{{"term", sub.empty() ? term : term + ": " + sub}, {"marked", marked}};
         }});
    add({"insert_index", "Insert index",
         "Insert the index (References > Insert Index) before a paragraph, from the entries marked with mark_index_entry. "
         "Page numbers fill in when Word opens the file or on render_pages/export_pdf.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"columns", integer("1-4, default 2.")}, {"track", kTrack}}, {"doc", "paragraph"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json o = Json::object();
             if (a.contains("columns")) o["columns"] = iarg(a, "columns", 2);
             int n = insert_index(s.doc, para(s, a).p, o, track(a));
             return Json{{"entries", n}, {"columns", iarg(a, "columns", 2)}};
         }});
    add({"insert_caption", "Caption",
         "Add a numbered caption (References > Insert Caption): \"Figure 1: text\", \"Table 2: text\". Numbers are "
         "real SEQ fields, so they renumber when captions are added or moved. Put it after a picture's paragraph, or "
         "before a table (table = n).",
         obj({{"doc", kDoc}, {"paragraph", str("Paragraph to put the caption next to.")},
              {"table", integer("Or a table number (1-based) to caption.")},
              {"label", str("Figure (default), Table, Equation, or your own one-word label.")},
              {"text", str("Caption text after the number.")}, {"separator", str("Between number and text (default \": \").")},
              {"where", enm({"after", "before"}, "Default: before a table, after anything else.")}, {"track", kTrack},
              {"author", str("Author of this change.")}},
             {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             pugi::xml_node ref;
             if (a.contains("table")) ref = table_at(s.doc, iarg(a, "table", 1));
             else if (a.contains("paragraph")) ref = para(s, a).p;
             else throw std::runtime_error("give paragraph or table");
             bool is_table = std::string_view(ref.name()) == "w:tbl";
             bool after = sarg(a, "where", is_table ? "before" : "after") == "after";
             auto p = insert_caption(s.doc, ref, after, sarg(a, "label", "Figure"), sarg(a, "text"), sarg(a, "separator"), track(a));
             return para_brief(s.doc, p);
         }});
    add({"list_reference_targets", "Cross-reference targets",
         "What a cross-reference can point at: headings, figure/table captions, bookmarks, footnotes and endnotes, "
         "with the target id to pass to insert_cross_reference.",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
             Json v = Json::array();
             for (auto& t : reference_targets(sess(S, a).doc)) {
                 Json j = {{"kind", t.kind}, {"target", t.id}, {"text", t.text}};
                 if (!t.label.empty()) j["label"] = t.label;
                 if (!t.para.empty() && t.para != t.id) j["paragraph"] = t.para;
                 v.push_back(j);
             }
             return v;
         }});
    add({"insert_cross_reference", "Cross-reference",
         "Insert a cross-reference (References > Cross-reference) to a heading, caption, bookmark or note: \"see Figure "
         "2\", \"as described in Section 3\", \"on page 5\". It's a REF/PAGEREF/NOTEREF field that stays right when "
         "the document changes. Targets come from list_reference_targets.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"target", str("Heading or caption paragraph id, bookmark name, or footnote:N / endnote:N.")},
              {"show", enm({"text", "number", "label_number", "page", "above_below"},
                           "What to show: the text, its number, \"Figure 2\" (label_number), the page, or above/below. Default text.")},
              {"link", boolean("Ctrl+click jumps to the target (default true).")}, {"position", integer("")}, {"after", str("")},
              {"track", kTrack}, {"author", str("Author of this change.")}},
             {"doc", "paragraph", "target"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             auto code = insert_cross_reference(s.doc, r.p, pos_in(r.p, a), need(a, "target"), sarg(a, "show", "text"),
                                                barg(a, "link", true), track(a));
             Json out = para_brief(s.doc, r.p);
             out["field"] = code;
             return out;
         }});
    add({"insert_equation", "Equation", "Insert an equation from LaTeX-like input: x^2, a_i, \\frac{a}{b}, \\sqrt{x}, \\sum_{i=1}^{n}, \\int, Greek letters.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"equation", str("")}, {"display", boolean("Own line (display) vs inline.")}, {"position", integer("")},
              {"track", kTrack}},
             {"doc", "paragraph", "equation"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             insert_equation(s.doc, r.p, pos_in(r.p, a), need(a, "equation"), barg(a, "display", false), track(a));
             return Json{{"equation", need(a, "equation")}, {"display", barg(a, "display", false)}, {"paragraph", para_id(r.p)}};
         }});
    add({"add_content_control", "Content control",
         "Wrap text in (or insert) a content control: richText, text, checkbox, dropDown, comboBox, date, picture. "
         "Used for forms and templates.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"type", enm({"richText", "text", "checkbox", "dropDown", "comboBox", "date", "picture"}, "")},
              {"match", str("Text to wrap (omit to insert an empty control at position).")}, {"position", integer("")}, {"tag", str("")}, {"title", str("")},
              {"items", arr(str(""), "Choices for dropDown/comboBox.")}, {"checked", boolean("")}, {"placeholder", str("")},
              {"date_format", str("e.g. M/d/yyyy")}, {"lock_delete", boolean("")}, {"lock_content", boolean("")}},
             {"doc", "paragraph"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             int b, e;
             if (a.contains("match")) std::tie(b, e) = range_in(r.p, a);
             else b = e = pos_in(r.p, a);
             Json o = Json::object();
             for (auto k : {"type", "tag", "items", "checked", "placeholder", "lock_delete", "lock_content"}) if (a.contains(k)) o[k] = a[k];
             if (a.contains("title")) o["alias"] = a["title"];
             if (a.contains("date_format")) o["format"] = a["date_format"];
             auto sdt = add_content_control(s.doc, r.p, b, e, o);
             return Json{{"id", xml::attr(sdt.child("w:sdtPr").child("w:id"), "w:val")}};
         }});
    add({"list_content_controls", "Content controls", "List content controls (fillable fields, checkboxes, dropdowns, dates) with tag, title, type and value.", obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
        Json v = Json::array();
        for (auto& c : list_content_controls(sess(S, a).doc)) {
            Json j = {{"id", c.id}, {"type", c.type}, {"text", c.text}};
            if (!c.tag.empty()) j["tag"] = c.tag;
            if (!c.alias.empty()) j["title"] = c.alias;
            if (c.type == "checkbox") j["checked"] = c.checked;
            v.push_back(j);
        }
        return v;
    }});
    add({"set_content_control", "Fill content control", "Set a content control's value (text, checkbox true/false, drop-down choice, date YYYY-MM-DD).",
         obj({{"doc", kDoc}, {"control", str("id, tag or title")}, {"value", {{"description", "string or boolean"}}}}, {"doc", "control", "value"}), false, false,
         [](Sessions& S, const Json& a) {
             set_content_control(mut(S, a).doc, need(a, "control"), a["value"]);
             for (auto& c : list_content_controls(sess(S, a).doc))
                 if (c.id == need(a, "control") || c.tag == need(a, "control") || c.alias == need(a, "control")) {
                     Json j = {{"id", c.id}, {"type", c.type}, {"text", c.text}};
                     if (c.type == "checkbox") j["checked"] = c.checked;
                     return j;
                 }
             return Json{{"control", need(a, "control")}};
         }});

    // ── layout: sections, headers/footers ────────────────────────────────
    add({"get_page_setup", "Page setup", "Sections with page size, orientation, margins, columns, header/footer parts.",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             Json v = Json::array();
             int i = 1;
             for (auto sp : sections(s.doc)) { auto j = section_json(s.doc, sp); j["section"] = i++; v.push_back(j); }
             return v;
         }});
    add({"set_page_setup", "Page setup",
         "Set page layout of a section (1-based, default all): orientation, paper (letter|a4|legal|a3|a5|b5|executive|tabloid), "
         "page_width/page_height, margins {top,bottom,left,right,header,footer,gutter} (pt or \"1in\"), columns, column_space, column_line, column_widths, "
         "page_borders, page_color, "
         "vertical_align, line_numbers, page_numbers {start, format}, title_page (different first page), type (section start).",
         obj({{"doc", kDoc}, {"section", integer("Omit for all sections.")},
              {"setup", {{"type", "object"}, {"description", "Lengths in pt or with units (\"1in\", \"2cm\")."},
                         {"properties", {{"orientation", enm({"portrait", "landscape"}, "")},
                                         {"paper", enm({"letter", "a4", "legal", "a3", "a5", "b5", "executive", "tabloid"}, "")},
                                         {"page_width", str("")}, {"page_height", str("")},
                                         {"margins", anyobj("{top,bottom,left,right,header,footer,gutter}")},
                                         {"columns", integer("")}, {"column_space", str("")},
                                         {"column_line", boolean("Line between columns.")},
                                         {"column_widths", arr(str(""), "Unequal columns: a width per column (pt or \"2in\").")},
                                         {"page_borders", anyobj("\"single\", {style, size, color} for all sides, or {top, left, bottom, right, offset_from: page|text}; null removes.")},
                                         {"page_color", str("Page background (whole document), RRGGBB or a name; null removes.")},
                                         {"vertical_align", enm({"top", "center", "both", "bottom"}, "")},
                                         {"line_numbers", anyobj("")}, {"page_numbers", anyobj("{start, format}")},
                                         {"title_page", boolean("Different first page.")},
                                         {"type", enm({"nextPage", "continuous", "evenPage", "oddPage"}, "Section start.")}}}}},
              {"even_odd_headers", boolean("")}, {"track", kTrack}},
             {"doc", "setup"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json props = obj_arg(a, "setup");
             if (a.contains("even_odd_headers")) props["even_odd_headers"] = a["even_odd_headers"];
             int n = (int)sections(s.doc).size();
             if (a.contains("section")) format_section(s.doc, iarg(a, "section", 1), props, track(a));
             else for (int i = 1; i <= n; i++) format_section(s.doc, i, props, track(a));
             // the resulting layout, so the model sees units and values took
             auto secs = sections(s.doc);
             Json j = section_json(s.doc, secs[a.contains("section") ? std::clamp(iarg(a, "section", 1), 1, (int)secs.size()) - 1 : 0]);
             j.erase("headers_footers");
             j["sections_changed"] = a.contains("section") ? 1 : n;
             return j;
         }});
    add({"set_header_footer", "Header / footer",
         "Set a header or footer's text for a section. Use {PAGE}, {NUMPAGES}, {DATE}, {SECTIONPAGES} placeholders for fields. "
         "which: default (all pages), first (first page, turns on Different First Page), even (even pages).",
         obj({{"doc", kDoc}, {"section", integer("Default 1.")}, {"kind", enm({"header", "footer"}, "")}, {"which", enm({"default", "first", "even"}, "")},
              {"text", str("Lines separated by \\n.")}, {"alignment", enm({"left", "center", "right"}, "")}},
             {"doc", "kind", "text"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             Json o = Json::object();
             if (a.contains("alignment")) o["alignment"] = a["alignment"];
             set_header_footer_text(s.doc, iarg(a, "section", 1), need(a, "kind"), sarg(a, "which", "default"), sarg(a, "text"), o);
             auto part = header_footer(s.doc, iarg(a, "section", 1), need(a, "kind"), sarg(a, "which", "default"), false);
             Json ps = Json::array();
             for (auto p : paragraphs(s.doc.pkg().xml(part).document_element())) ps.push_back(para_brief(s.doc, p));
             return Json{{"part", part}, {"paragraphs", ps}, {"hint", "edit further with paragraph = \"" + part + "#<id>\""}};
         }});
    add({"remove_header_footer", "Remove header / footer", "Remove a header or footer (default, first page or even pages) from a section.",
         obj({{"doc", kDoc}, {"section", integer("")}, {"kind", enm({"header", "footer"}, "")}, {"which", enm({"default", "first", "even"}, "")}}, {"doc", "kind"}),
         false, true, [](Sessions& S, const Json& a) {
             remove_header_footer(mut(S, a).doc, iarg(a, "section", 1), need(a, "kind"), sarg(a, "which", "default"));
             return Json{{"removed", sarg(a, "which", "default") + " " + need(a, "kind")}, {"section", iarg(a, "section", 1)}};
         }});
    add({"set_watermark", "Watermark",
         "Put a text watermark behind every page (Design > Watermark), e.g. DRAFT or CONFIDENTIAL. remove: true takes it off.",
         obj({{"doc", kDoc}, {"text", str("Watermark text.")}, {"remove", boolean("Remove the watermark instead.")},
              {"layout", enm({"diagonal", "horizontal"}, "Default diagonal.")}, {"color", str("RRGGBB or a name. Default silver.")},
              {"font", str("Default Calibri.")}, {"size", num("Points; default fits the page width.")},
              {"opacity", num("0-1, default 0.5.")}},
             {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             if (barg(a, "remove", false)) return Json{{"removed", remove_watermark(s.doc)}};
             int n = set_watermark(s.doc, need(a, "text"), a);
             return Json{{"watermark", need(a, "text")}, {"headers", n}, {"layout", sarg(a, "layout", "diagonal")}};
         }});
    add({"read_header_footer", "Read header / footer", "Read a header or footer: its paragraphs with ids you can edit (paragraph = \"part#id\").", obj({{"doc", kDoc}, {"section", integer("")}, {"kind", enm({"header", "footer"}, "")},
                                                                {"which", enm({"default", "first", "even"}, "")}},
                                                               {"doc", "kind"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             auto part = header_footer(s.doc, iarg(a, "section", 1), need(a, "kind"), sarg(a, "which", "default"), false);
             if (part.empty()) return Json{{"exists", false}};
             auto root = s.doc.pkg().xml(part).document_element();
             assign_missing_para_ids(s.doc, root, part);
             Json ps = Json::array();
             for (auto p : paragraphs(root)) ps.push_back(para_brief(s.doc, p));
             return Json{{"exists", true}, {"part", part}, {"paragraphs", ps}};
         }});

    // ── document properties / settings / protection ─────────────────────
    add({"get_properties", "Properties", "Core (title, author, ...), custom, and application properties.", obj({{"doc", kDoc}}, {"doc"}), true, false,
         [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             return Json{{"core", core_properties(s.doc)}, {"custom", custom_properties(s.doc)}, {"app", app_properties(s.doc)}};
         }});
    add({"set_properties", "Set properties",
         "Set core properties (title, subject, creator, keywords, description, category, content_status, language, ...) and/or "
         "custom properties (name -> string/number/bool; null removes).",
         obj({{"doc", kDoc}, {"core", anyobj("")}, {"custom", anyobj("")}}, {"doc"}), false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             if (a.contains("core")) set_core_properties(s.doc, obj_arg(a, "core"));
             if (a.contains("custom")) {
                 Json custom = obj_arg(a, "custom");  // named: .items() on a temporary dangles before C++23 P2718
                 for (auto& [k, v] : custom.items()) set_custom_property(s.doc, k, v);
             }
             return Json{{"core", core_properties(s.doc)}, {"custom", custom_properties(s.doc)}};
         }});
    add({"get_settings", "Settings", "Document settings: track changes, protection, compatibility mode, tab stops, zoom, ...",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) { return settings_json(sess(S, a).doc); }});
    add({"set_settings", "Set settings",
         "track_changes, update_fields_on_open, even_odd_headers, mirror_margins, gutter_at_top, default_tab_stop (pt), zoom, "
         "show_markup, show_formatting_changes, do_not_track_moves, do_not_track_formatting, auto_hyphenation, "
         "embed_true_type_fonts, hide_spelling_errors, hide_grammar_errors, compatibility_mode (15 = Word 2013+).",
         obj({{"doc", kDoc}, {"settings", anyobj("")}}, {"doc", "settings"}), false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             set_settings(s.doc, obj_arg(a, "settings"));
             return settings_json(s.doc);
         }});
    add({"protect_document", "Restrict editing",
         "Restrict editing (Review > Restrict Editing): readOnly, comments, trackedChanges, forms. Optional password hashed exactly "
         "as Word does (SHA-512, 100000 spins).",
         obj({{"doc", kDoc}, {"mode", enm({"readOnly", "comments", "trackedChanges", "forms"}, "")}, {"password", str("")}}, {"doc", "mode"}), false, false,
         [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             protect(s.doc, need(a, "mode"), sarg(a, "password"));
             return protection_info(s.doc);
         }});
    add({"unprotect_document", "Stop protection", "Remove editing restrictions (password required if one was set).",
         obj({{"doc", kDoc}, {"password", str("")}}, {"doc"}), false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             unprotect(s.doc, sarg(a, "password"));
             return protection_info(s.doc);
         }});

    // ── quality ──────────────────────────────────────────────────────────
    add({"validate_document", "Validate", "Check every part against the full OOXML schema (Office 2007-365 extensions) and the package's relationships.",
         obj({{"doc", kDoc}}, {"doc"}), true, false, [](Sessions& S, const Json& a) { return validate_doc(sess(S, a).doc); }});
    add({"get_xml", "Raw XML", "Inspect the raw XML of a part or one paragraph (for diagnosing unusual content).",
         obj({{"doc", kDoc}, {"part", str("Part name, e.g. /word/styles.xml (default main document).")}, {"paragraph", kPara}, {"max_chars", integer("Default 20000.")}},
             {"doc"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             std::string x;
             if (a.contains("paragraph")) x = xml::to_string(para(s, a).p, true);
             else x = xml::to_string(s.doc.pkg().xml(sarg(a, "part", s.doc.main())).document_element(), true);
             std::size_t mx = iarg(a, "max_chars", 20000);
             bool cut = x.size() > mx;
             if (cut) x = x.substr(0, mx);
             return Json{{"xml", x}, {"truncated", cut}, {"parts", s.doc.pkg().part_names()}};
         }});
    add({"set_paragraph_xml", "Replace paragraph XML",
         "Expert escape hatch for things no other tool covers: replace a paragraph with raw WordprocessingML (one or "
         "more w:p/w:tbl). Prefixes w, r, a, m, wp, w14... are predeclared. Properties are put in the schema's order "
         "for you; anything still invalid is rejected with the reason. Prefer format_paragraph / format_text.",
         obj({{"doc", kDoc}, {"paragraph", kPara}, {"xml", str("")}}, {"doc", "paragraph", "xml"}), false, false, [](Sessions& S, const Json& a) {
             auto& s = mut(S, a);
             auto r = para(s, a);
             std::string old_id = para_id(r.p);
             pugi::xml_document frag;
             std::string err;
             if (!xml::parse_fragment(need(a, "xml"), frag, &err)) throw std::runtime_error("bad XML: " + err);
             // Word rejects properties out of schema order (w:jc before
             // w:spacing...). That's bookkeeping, not intent: fix it here.
             std::function<void(pugi::xml_node)> order = [&](pugi::xml_node n) {
                 for (auto c : n.children())
                     if (c.type() == pugi::node_element) order(c);
                 std::string_view q = n.name();
                 if (q.size() > 2 && q.substr(q.size() - 2) == "Pr") schema::sort_children(n);
             };
             for (auto c : frag.children()) order(c);
             // validate in place on a copy of the part; reject only issues we introduce
             auto& part = s.doc.pkg().xml(r.part);
             std::size_t before = schema::validate(part.document_element(), {100000}).size();
             pugi::xml_document trial;
             trial.reset(part);
             auto parent = r.p.parent();
             std::vector<pugi::xml_node> added;
             for (auto c : frag.children()) added.push_back(parent.insert_copy_before(c, r.p));
             parent.remove_child(r.p);
             auto issues = schema::validate(part.document_element(), {100000});
             if (issues.size() > before) {
                 part.reset(trial);
                 throw std::runtime_error("rejected: " + issues[0].message + " (at " + issues[0].path + ")");
             }
             xml::fix_declarations(part.document_element());
             Json ids = Json::array();
             for (auto n : added)
                 if (std::string_view(n.name()) == "w:p") {
                     if (para_id(n).empty()) xml::set_attr(n, "w14:paraId", s.doc.new_hex_id());
                     ids.push_back(para_brief(s.doc, n));
                 }
             return Json{{"replaced", old_id}, {"paragraphs", ids}};
         }});

    // ── seeing the page ──
    add({"render_pages", "See pages",
         "Render pages of the document as images so you can see the layout: where pictures, tables and page breaks "
         "fall, how a heading looks. Returns PNG images (and the page count). Uses LibreOffice; unsaved edits are "
         "included.",
         obj({{"doc", kDoc}, {"page", integer("First page (1-based, default 1).")}, {"to_page", integer("Last page (default = page; at most 20).")},
              {"dpi", integer("Resolution, 30-300 (default 80; 150 to read small text).")}},
             {"doc"}),
         true, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             int total = 0;
             int first = iarg(a, "page", 1);
             auto pages = render_pages(s.doc, first, iarg(a, "to_page", first), iarg(a, "dpi", 80), total);
             Json out = {{"pages", total}, {"rendered", Json::array()}, {"_images", Json::array()}};
             for (auto& p : pages) {
                 out["rendered"].push_back(p.page);
                 out["_images"].push_back({{"data", util::base64_encode(p.png)}, {"mime", "image/png"}});
             }
             return out;
         }});
    add({"export_pdf", "Save as PDF",
         "Export the document to PDF (File > Save As > PDF), with the current unsaved edits. Uses LibreOffice.",
         obj({{"doc", kDoc}, {"path", str("Where to write the .pdf (default: next to the document).")}}, {"doc"}),
         false, false, [](Sessions& S, const Json& a) {
             auto& s = sess(S, a);
             std::string path = sarg(a, "path");
             if (path.empty()) {
                 auto p = util::u8path(s.path);
                 p.replace_extension(".pdf");
                 path = util::path_utf8(p);
             }
             path = util::normalize_path(path);
             auto pdf = export_pdf(s.doc);
             util::write_file_atomic(path, pdf);
             return Json{{"saved", path}, {"bytes", pdf.size()}};
         }});

    // ── discovery ──────────────────────────────────────────────────────────
    // Clients that rank or cap tools show a model only a few of these. help
    // maps every task to its tool; run_tool runs one the client didn't show.
    // Both read the full catalog through `catalog` (set once T is complete).
    auto catalog = std::make_shared<std::vector<ToolDef>>();
    add({"help", "What can this do / which tool",
         "Help for Word documents (docx files): lists everything this can do, by task, and which tool does it. "
         "Open, create, read, write and edit text and paragraphs; format font, size, bold, headings, styles; "
         "tables, rows, cells; pictures; lists and bullets; headers, footers, page numbers, margins; comments; "
         "track changes; footnotes; links; table of contents. Give tool names or tasks (comma-separated) to see how to "
         "call them. Only needed when a Word tool isn't in your list; tools you can see, call directly.",
         obj({{"topic", str("Tool names or tasks, comma-separated (\"insert_table, format_text\", \"add a comment\"). Omit for everything.")}}), true, false,
         [catalog](Sessions&, const Json& a) { return Json{{"_text", help_result(*catalog, sarg(a, "topic"))}}; }});
    add({"run_tool", "Run any docx tool",
         "Run any Word document tool by name, including ones your client didn't list (find them with help). Same "
         "arguments as calling the tool directly: edit, format, tables, comments, track changes, headers, anything.",
         obj({{"tool", str("Tool name from help, e.g. insert_table.")}, {"arguments", anyobj("That tool's arguments.")}},
             {"tool"}),
         false, false, [catalog](Sessions& S, const Json& a) -> Json {
             std::string name = need(a, "tool");
             if (name == "run_tool" || name == "help") throw std::runtime_error("call " + name + " directly");
             for (auto& t : *catalog)
                 if (t.name == name) return call_tool(t, S, a.contains("arguments") ? a["arguments"] : Json::object());
             std::string best;
             std::size_t bd = 4;
             for (auto& t : *catalog) {
                 // closest name by edit distance, for a typo
                 auto& x = t.name;
                 std::vector<std::size_t> pr(x.size() + 1), cu(x.size() + 1);
                 for (std::size_t j = 0; j <= x.size(); j++) pr[j] = j;
                 for (std::size_t i = 1; i <= name.size(); i++) {
                     cu[0] = i;
                     for (std::size_t j = 1; j <= x.size(); j++)
                         cu[j] = std::min({pr[j] + 1, cu[j - 1] + 1, pr[j - 1] + (name[i - 1] == x[j - 1] ? 0 : 1)});
                     std::swap(pr, cu);
                 }
                 if (pr[x.size()] < bd) { bd = pr[x.size()]; best = x; }
             }
             throw std::runtime_error("unknown tool '" + name + "'" + (best.empty() ? "" : " (did you mean '" + best + "'?)") +
                                      "; help lists every tool by task");
         }});
    *catalog = T;

    return T;
}

}  // namespace docx::server
