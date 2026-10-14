#include "mcp/discover.hpp"

#include <algorithm>
#include <set>

#include "mcp/tools.hpp"
#include "util/util.hpp"

namespace docx::server {

const std::vector<TaskGroup>& task_groups() {
    static const std::vector<TaskGroup> g = {
        {"Files", "open, create a new blank document, save, save as, close, undo",
         {"open_document", "create_document", "save_document", "close_document", "undo", "list_open_documents"}},
        {"Reading", "read the document, see all paragraphs and their ids, get the text or markdown, headings, word count, search",
         {"read_document", "get_text", "get_paragraph", "get_outline", "find_text", "get_statistics"}},
        {"Text", "type, insert, replace, delete or find-and-replace text",
         {"insert_text", "replace_text", "delete_text", "find_replace"}},
        {"Paragraphs", "add, delete, split, merge or move paragraphs",
         {"insert_paragraph", "delete_paragraph", "split_paragraph", "merge_paragraphs", "move_paragraphs"}},
        {"Formatting", "bold, italic, font, size, color, highlight; alignment, spacing, indents; styles like Heading 1",
         {"format_text", "format_paragraph", "apply_style", "clear_formatting", "list_styles", "get_style", "modify_style",
          "delete_style"}},
        {"Lists", "bullets, numbering, list levels, restart numbering",
         {"apply_list", "set_list_level", "remove_list", "restart_numbering"}},
        {"Tables", "insert a table, read it, change cell text, add or delete rows and columns, merge or split cells, borders, shading",
         {"insert_table", "read_table", "set_cell", "table_rows", "table_columns", "merge_cells", "split_cell", "format_table",
          "delete_table"}},
        {"Insert", "pictures, page and section breaks, links, bookmarks, footnotes, endnotes, fields, table of contents, equations, captions, cross-references",
         {"insert_picture", "list_pictures", "format_picture", "delete_picture", "insert_break", "insert_hyperlink", "list_hyperlinks", "remove_hyperlinks", "add_bookmark",
          "list_bookmarks", "delete_bookmark", "insert_note", "list_notes", "delete_note", "insert_field", "list_fields",
          "update_fields", "insert_toc", "insert_equation", "insert_caption", "insert_cross_reference",
          "list_reference_targets"}},
        {"Forms", "content controls: text boxes to fill, checkboxes, dropdowns, dates",
         {"add_content_control", "list_content_controls", "set_content_control"}},
        {"Layout", "margins, orientation, paper size, columns, page borders, page color, watermark, headers, footers, page numbers",
         {"get_page_setup", "set_page_setup", "set_header_footer", "read_header_footer", "remove_header_footer", "set_watermark"}},
        {"Review", "track changes, accept or reject changes, compare two documents, comments and replies",
         {"set_track_changes", "list_revisions", "accept_revisions", "reject_revisions", "compare_documents", "add_comment",
          "list_comments", "reply_comment", "resolve_comment", "edit_comment", "delete_comment"}},
        {"Document", "title, author, properties, settings, restrict editing, protect or unprotect",
         {"get_properties", "set_properties", "get_settings", "set_settings", "protect_document", "unprotect_document"}},
        {"Seeing the page", "see what pages look like, save as PDF",
         {"render_pages", "export_pdf"}},
        {"Expert", "check validity, raw XML",
         {"validate_document", "get_xml", "set_paragraph_xml"}},
    };
    return g;
}

namespace {

std::string first_sentence(const std::string& s) {
    auto p = s.find(". ");
    return p == std::string::npos ? s : s.substr(0, p + 1);
}

// "doc, paragraph, text, [position], [track]": required first, optional in brackets
std::string arg_line(const ToolDef& t) {
    std::set<std::string> req;
    for (auto& r : t.schema.value("required", Json::array())) req.insert(r.get<std::string>());
    std::string out;
    for (auto& r : t.schema.value("required", Json::array())) out += (out.empty() ? "" : ", ") + r.get<std::string>();
    if (t.schema.contains("properties"))
        for (auto& [k, _] : t.schema["properties"].items())
            if (!req.count(k) && k != "author") out += (out.empty() ? "[" : ", [") + k + "]";
    return out;
}

// One short line per argument: "  text (string, required): what it is".
// What a model needs to call the tool, without JSON Schema noise.
std::string arg_lines(const ToolDef& t) {
    std::set<std::string> req;
    for (auto& r : t.schema.value("required", Json::array())) req.insert(r.get<std::string>());
    auto type_of = [](const Json& p) -> std::string {
        if (p.contains("enum")) {
            std::string s;
            for (auto& e : p["enum"]) s += (s.empty() ? "" : "|") + e.get<std::string>();
            return s;
        }
        if (!p.contains("type")) return "string or object";
        auto& ty = p["type"];
        if (ty.is_array()) {
            std::string s;
            for (auto& e : ty) s += (s.empty() ? "" : " or ") + e.get<std::string>();
            return s;
        }
        std::string s = ty.get<std::string>();
        if (s == "array" && p.contains("items") && p["items"].contains("type") && p["items"]["type"].is_string())
            return "list of " + p["items"]["type"].get<std::string>();
        if (s == "object" && p.contains("properties")) {
            std::string keys;
            for (auto& [k, _] : p["properties"].items()) keys += (keys.empty() ? "" : ", ") + k;
            return "object: " + keys;
        }
        return s;
    };
    std::string out;
    if (!t.schema.contains("properties")) return out;
    // required first, then the rest in schema order
    std::vector<std::string> order;
    for (auto& r : t.schema.value("required", Json::array())) order.push_back(r.get<std::string>());
    for (auto& [k, _] : t.schema["properties"].items())
        if (!req.count(k) && k != "author") order.push_back(k);
    for (auto& k : order) {
        auto& p = t.schema["properties"][k];
        std::string d = p.value("description", "");
        if (d.size() > 160) d = first_sentence(d);
        out += "  " + k + " (" + type_of(p) + (req.count(k) ? ", required" : "") + ")" + (d.empty() ? "" : ": " + d) + "\n";
    }
    return out;
}


}  // namespace

namespace {
// Tools matching one topic: an exact tool name, an area, or words.
std::vector<const ToolDef*> match_topic(const std::vector<ToolDef>& all, const std::string& topic) {
    auto find = [&](const std::string& n) -> const ToolDef* {
        for (auto& t : all)
            if (t.name == n) return &t;
        return nullptr;
    };
    std::vector<const ToolDef*> hits;
    if (auto t = find(topic)) return {t};
    // "insert paragraph" / "insert-paragraph" mean insert_paragraph
    {
        std::string u = topic;
        for (auto& c : u) if (c == ' ' || c == '-') c = '_';
        if (auto t = find(u)) return {t};
    }
    for (auto& g : task_groups())
        if (util::lower(g.area) == topic) {
            for (auto n : g.tools)
                if (auto t = find(n)) hits.push_back(t);
            return hits;
        }
    // what people say -> the words our tools use
    static const std::vector<std::pair<const char*, const char*>> syn = {
        {"new", "create"}, {"blank", "create"}, {"bold", "format_text"}, {"italic", "format_text"},
        {"center", "format_paragraph"}, {"align", "format_paragraph"}, {"indent", "format_paragraph"},
        {"spacing", "format_paragraph"}, {"shade", "format_table"}, {"border", "format_table"}, {"heading", "apply_style insert_paragraph outline"},
        {"size", "format_text"}, {"underline", "format_text"}, {"font", "format_text"}, {"color", "format_text"},
        {"bullet", "list"}, {"numbering", "list"}, {"row", "table_rows"}, {"column", "table_columns"},
        {"cell", "set_cell"}, {"image", "picture"}, {"photo", "picture"}, {"logo", "picture"},
        {"wrap", "format_picture"}, {"resize", "format_picture"}, {"link", "hyperlink"}, {"url", "hyperlink"},
        {"footnote", "note"}, {"endnote", "note"}, {"redline", "revision"}, {"change", "revision"},
        {"margin", "page_setup"}, {"landscape", "page_setup"}, {"orientation", "page_setup"}, {"paper", "page_setup"},
        {"page number", "header_footer"}, {"page num", "header_footer"}, {"footer", "header_footer"}, {"header", "header_footer"}, {"footer", "header_footer"},
        {"contents", "toc"}, {"password", "protect"}, {"lock", "protect"}, {"title", "properties"},
        {"metadata", "properties"}, {"read", "read_document"}, {"word count", "statistics"}, {"search", "find_text"},
        {"replace", "replace"}, {"equation", "equation"}, {"formula", "equation"}, {"checkbox", "content_control"},
        {"dropdown", "content_control"}, {"pdf", "export_pdf"}, {"render", "render_pages"}, {"look", "render_pages"},
        {"preview", "render_pages"}, {"caption", "caption"}, {"cross", "cross_reference"}};
    // a guessed tool name ("insert_page_number", "add_footnote") is words too
    std::string expanded = topic;
    for (auto& c : expanded) if (c == '_' || c == '-') c = ' ';
    std::string spaced = expanded;
    for (auto& [k, v] : syn)
        if (spaced.find(k) != std::string::npos) expanded += std::string(" ") + v;
    std::vector<std::string> words;
    for (auto& w : util::split(expanded, ' '))
        if (w.size() >= 3 && w != "the" && w != "and" && w != "for" && w != "with")
            words.push_back(w.back() == 's' && w.size() > 3 ? w.substr(0, w.size() - 1) : w);
    // verbs say less than nouns: "insert_page_number" is about page numbers
    auto verb = [](const std::string& w) {
        return w == "add" || w == "insert" || w == "set" || w == "get" || w == "new" || w == "make" ||
               w == "create" || w == "put" || w == "change" || w == "update";
    };
    std::vector<std::pair<int, const ToolDef*>> scored;
    for (auto& g : task_groups())
        for (auto n : g.tools) {
            auto t = find(n);
            if (!t) continue;
            std::string own = util::lower(t->title + " " + t->description);
            std::string grp = util::lower(std::string(g.area) + " " + g.says);
            int sc = 0;
            for (auto& w : words) {
                int k = verb(w) ? 1 : 3;
                if (t->name.find(w) != std::string::npos) sc += 2 * k;
                else if (own.find(w) != std::string::npos) sc += k;
                else if (grp.find(w) != std::string::npos) sc += k > 1 ? 1 : 0;
            }
            if (sc > 1) scored.push_back({sc, t});
        }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < scored.size() && i < 6; i++) hits.push_back(scored[i].second);
    return hits;
}
}  // namespace

std::string help_result(const std::vector<ToolDef>& all, const std::string& topic_in) {
    std::string topic = util::lower(util::trim(topic_in));
    std::string out;
    if (topic.empty()) {
        out = "docx-mcp: Word documents by task. Call a tool directly, or through run_tool if it isn't in your list.\n"
              "help {topic: \"insert_table, format_text\"} shows how to call tools (several at once, comma-separated).\n\n";
        for (auto& g : task_groups()) {
            out += std::string(g.area) + " (" + g.says + "):\n  ";
            bool first = true;
            for (auto n : g.tools)
                for (auto& t : all)
                    if (t.name == n) { out += (first ? "" : ", ") + t.name; first = false; }
            out += "\n";
        }
        return out;
    }
    // several topics at once: "insert_table, format_text, add a comment"
    std::vector<std::string> topics;
    for (auto& part : util::split(topic, ','))
        if (auto t = util::trim(part); !t.empty()) topics.push_back(t);
    std::vector<const ToolDef*> exact, other;
    std::set<std::string> seen;
    for (auto& t : topics) {
        auto hits = match_topic(all, t);
        // the best hit for each topic gets its full arguments, so one help
        // call is enough to start working
        for (std::size_t i = 0; i < hits.size(); i++)
            if (seen.insert(hits[i]->name).second) (i == 0 ? exact : other).push_back(hits[i]);
        if (hits.empty()) out += "nothing matched '" + t + "'.\n";
    }
    // exact tools: everything needed to call them
    for (auto t : exact) {
        out += t->name + ": " + (t->description.empty() ? t->title : t->description) + "\n" + arg_lines(*t) + "\n";
    }
    // searches: one line each
    if (!other.empty()) {
        if (!exact.empty()) out += "Related:\n";
        for (auto t : other)
            out += "  " + t->name + "(" + arg_line(*t) + "): " +
                   first_sentence(t->description.empty() ? t->title : t->description) + "\n";
        out += "help {topic: \"<tool name>\"} for a tool's arguments.\n";
    }
    if (exact.empty() && other.empty()) out += "help with no topic lists every area.\n";
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

namespace {
std::string arg(const std::vector<std::pair<std::string, std::string>>& a, const char* k, const char* def = "") {
    for (auto& [n, v] : a)
        if (n == k && !v.empty()) return v;
    return def;
}
}  // namespace

const std::vector<Workflow>& workflows() {
    static const std::vector<Workflow> w = {
        {"review_document", "Review a document",
         "Read a document and leave review comments and tracked edits, like a careful reviewer in Word.",
         {{"path", "The .docx to review.", true}, {"focus", "What to look for (clarity, legal risk, tone...).", false},
          {"author", "Reviewer name on comments and changes.", false}},
         [](const std::vector<std::pair<std::string, std::string>>& a) -> std::string {
             return "Review the Word document at " + arg(a, "path") +
                    ".\n\n1. open_document with author \"" + arg(a, "author", "Reviewer") +
                    "\", then read_document (page through with offset if there are more paragraphs).\n"
                    "2. set_track_changes on.\n"
                    "3. For small fixes (typos, wording) use replace_text with match, so they show as tracked changes.\n"
                    "4. For anything that needs the author's decision, add_comment on the exact text (match).\n"
                    "5. save_document.\n\nFocus on: " + arg(a, "focus", "clarity, correctness and consistency") +
                    ".\nEnd with a short summary of what you changed and what you commented on.";
         }},
        {"draft_document", "Draft a new document",
         "Write a new Word document with real headings, lists and tables (not plain text).",
         {{"path", "Where to save the new .docx.", true}, {"brief", "What the document should say.", true}},
         [](const std::vector<std::pair<std::string, std::string>>& a) -> std::string {
             return "Create a Word document at " + arg(a, "path") + " from this brief:\n\n" + arg(a, "brief") +
                    "\n\nUse create_document, then insert_paragraph with style \"Title\", \"Heading 1\", \"Heading 2\" for "
                    "structure, apply_list for bullet or numbered points, insert_table for tabular content, and "
                    "set_header_footer for a page-number footer (\"Page {PAGE} of {NUMPAGES}\"). Save with save_document.";
         }},
        {"redline", "Compare two versions",
         "Show the differences between two versions of a document as tracked changes.",
         {{"original", "The earlier .docx.", true}, {"revised", "The later .docx.", true}, {"output", "Where to write the comparison.", false}},
         [](const std::vector<std::pair<std::string, std::string>>& a) -> std::string {
             return "Compare " + arg(a, "original") + " with " + arg(a, "revised") + " using compare_documents, "
                    "writing the result to " + arg(a, "output", "comparison.docx") +
                    ". Then open it, list_revisions, and summarize the substantive changes in plain words.";
         }},
        {"fill_template", "Fill a template",
         "Fill placeholders, content controls and fields in a template document.",
         {{"path", "The template .docx.", true}, {"values", "The values, as JSON or plain \"name: value\" lines.", true},
          {"output", "Where to save the filled copy.", false}},
         [](const std::vector<std::pair<std::string, std::string>>& a) -> std::string {
             return "Fill the template " + arg(a, "path") + " with these values:\n\n" + arg(a, "values") +
                    "\n\n1. open_document, then list_content_controls and list_fields to find the slots; "
                    "find_text for placeholders like [Name] or {{name}}.\n2. set_content_control for controls, "
                    "find_replace for text placeholders.\n3. update_fields, then save_document" +
                    (arg(a, "output").empty() ? std::string(".") : " with path " + arg(a, "output") + ".") +
                    "\nSay which values had no slot.";
         }},
    };
    return w;
}

std::string guide_markdown(const std::vector<ToolDef>& all) {
    std::string md = "# docx-mcp tools by task\n\nCall a tool by name, or through run_tool if your client "
                     "doesn't list it. help {topic} shows arguments.\n";
    for (auto& g : task_groups()) {
        md += std::string("\n## ") + g.area + "\n\n" + g.says + "\n\n";
        for (auto n : g.tools)
            for (auto& t : all)
                if (t.name == n)
                    md += "- `" + t.name + "` (" + arg_line(t) + "): " +
                          first_sentence(t.description.empty() ? t.title : t.description) + "\n";
    }
    return md;
}

}  // namespace docx::server
