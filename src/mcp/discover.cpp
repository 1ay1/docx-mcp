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
        {"Tables", "insert a table, read it, change cell text, add or delete rows and columns, merge cells, borders, shading",
         {"insert_table", "read_table", "set_cell", "table_rows", "table_columns", "merge_cells", "format_table",
          "delete_table"}},
        {"Insert", "pictures, page and section breaks, links, bookmarks, footnotes, endnotes, fields, table of contents, equations, captions, cross-references",
         {"insert_picture", "insert_break", "insert_hyperlink", "list_hyperlinks", "remove_hyperlinks", "add_bookmark",
          "list_bookmarks", "delete_bookmark", "insert_note", "list_notes", "delete_note", "insert_field", "list_fields",
          "update_fields", "insert_toc", "insert_equation", "insert_caption", "insert_cross_reference",
          "list_reference_targets"}},
        {"Forms", "content controls: text boxes to fill, checkboxes, dropdowns, dates",
         {"add_content_control", "list_content_controls", "set_content_control"}},
        {"Layout", "margins, orientation, paper size, columns, headers, footers, page numbers",
         {"get_page_setup", "set_page_setup", "set_header_footer", "read_header_footer", "remove_header_footer"}},
        {"Review", "track changes, accept or reject changes, compare two documents, comments and replies",
         {"set_track_changes", "list_revisions", "accept_revisions", "reject_revisions", "compare_documents", "add_comment",
          "list_comments", "reply_comment", "resolve_comment", "edit_comment", "delete_comment"}},
        {"Document", "title, author, properties, settings, restrict editing, protect or unprotect",
         {"get_properties", "set_properties", "get_settings", "set_settings", "protect_document", "unprotect_document"}},
        {"Expert", "check validity, raw XML",
         {"validate_document", "get_xml", "set_paragraph_xml"}},
    };
    return g;
}

namespace {

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

std::string first_sentence(const std::string& s) {
    auto p = s.find(". ");
    return p == std::string::npos ? s : s.substr(0, p + 1);
}

}  // namespace

Json help_result(const std::vector<ToolDef>& all, const std::string& topic_in) {
    auto find = [&](const std::string& n) -> const ToolDef* {
        for (auto& t : all)
            if (t.name == n) return &t;
        return nullptr;
    };
    std::string topic = util::lower(util::trim(topic_in));
    Json out;
    if (topic.empty()) {
        Json areas = Json::array();
        for (auto& g : task_groups()) {
            Json tools = Json::array();
            for (auto n : g.tools)
                if (find(n)) tools.push_back(n);
            areas.push_back({{"area", g.area}, {"for", g.says}, {"tools", tools}});
        }
        out["areas"] = areas;
        out["next"] = "help {topic} shows a tool's arguments, e.g. help {\"topic\": \"tables\"} or {\"topic\": \"insert_table\"}. "
                      "If a tool isn't in your list, call it with run_tool {\"tool\": name, \"arguments\": {...}}.";
        return out;
    }
    // exact tool, an area, or words matched against what each tool is for
    std::vector<const ToolDef*> hits;
    if (auto t = find(topic)) hits.push_back(t);
    for (auto& g : task_groups()) {
        if (!hits.empty()) break;
        if (util::lower(g.area) == topic)
            for (auto n : g.tools)
                if (auto t = find(n)) hits.push_back(t);
    }
    if (hits.empty()) {
        // what people say -> the words our tools use
        static const std::vector<std::pair<const char*, const char*>> syn = {
            {"new", "create"}, {"blank", "create"}, {"bold", "format_text"}, {"italic", "format_text"}, {"center", "format_paragraph"}, {"align", "format_paragraph"},
            {"indent", "format_paragraph"}, {"spacing", "format_paragraph"}, {"shade", "format_table"}, {"border", "format_table"},
            {"heading", "outline"}, {"size", "format_text"},
            {"underline", "format_text"}, {"font", "format_text"}, {"color", "format_text"},
            {"bullet", "list"}, {"numbering", "list"}, {"row", "table_rows"}, {"column", "table_columns"},
            {"cell", "set_cell"}, {"image", "picture"}, {"photo", "picture"}, {"logo", "picture"},
            {"link", "hyperlink"}, {"url", "hyperlink"}, {"footnote", "note"}, {"endnote", "note"},
            {"redline", "revision"}, {"change", "revision"}, {"margin", "page_setup"}, {"landscape", "page_setup"},
            {"orientation", "page_setup"}, {"paper", "page_setup"}, {"page number", "header_footer"},
            {"header", "header_footer"}, {"footer", "header_footer"}, {"contents", "toc"}, {"password", "protect"},
            {"lock", "protect"}, {"title", "properties"}, {"metadata", "properties"}, {"read", "read_document"},
            {"word count", "statistics"}, {"search", "find_text"}, {"replace", "replace"}, {"equation", "equation"},
            {"formula", "equation"}, {"checkbox", "content_control"}, {"dropdown", "content_control"}};
        std::string expanded = topic;
        for (auto& [k, v] : syn)
            if (topic.find(k) != std::string::npos) expanded += std::string(" ") + v;
        std::vector<std::string> words;
        for (auto& w : util::split(expanded, ' '))
            if (w.size() >= 3) words.push_back(w.back() == 's' && w.size() > 3 ? w.substr(0, w.size() - 1) : w);
        std::vector<std::pair<int, const ToolDef*>> scored;
        for (auto& g : task_groups())
            for (auto n : g.tools) {
                auto t = find(n);
                if (!t) continue;
                std::string own = util::lower(t->title + " " + t->description);
                std::string grp = util::lower(std::string(g.area) + " " + g.says);
                int s = 0;
                for (auto& w : words) {
                    if (t->name.find(w) != std::string::npos) s += 6;
                    else if (own.find(w) != std::string::npos) s += 2;
                    else if (grp.find(w) != std::string::npos) s += 1;
                }
                if (s > 1) scored.push_back({s, t});
            }
        std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < scored.size() && i < 8; i++) hits.push_back(scored[i].second);
    }
    if (hits.empty()) {
        out["tools"] = Json::array();
        out["next"] = "nothing matched '" + topic_in + "'; call help with no topic to see every area";
        return out;
    }
    Json tools = Json::array();
    for (auto t : hits)
        tools.push_back({{"tool", t->name}, {"does", first_sentence(t->description.empty() ? t->title : t->description)},
                         {"args", arg_line(*t)}});
    out["tools"] = tools;
    if (hits.size() == 1) out["schema"] = hits[0]->schema;
    out["next"] = "call a tool directly, or through run_tool {\"tool\": name, \"arguments\": {...}} if it isn't in your list";
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
