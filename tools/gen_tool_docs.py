#!/usr/bin/env python3
"""Generate docs/tools.md from `docx-mcp --list-tools`.
usage: gen_tool_docs.py <docx-mcp binary>"""
import json
import os
import subprocess
import sys

GROUPS = [
    ("Documents", ["open_document", "create_document", "save_document", "close_document", "list_open_documents", "undo", "batch"]),
    ("Reading", ["read_document", "get_text", "get_outline", "get_paragraph", "find_text", "get_statistics"]),
    ("Editing text", ["insert_text", "replace_text", "delete_text", "find_replace", "insert_paragraph", "insert_markdown", "insert_html", "delete_paragraph",
                      "split_paragraph", "merge_paragraphs", "move_paragraphs"]),
    ("Formatting", ["format_text", "format_paragraph", "apply_style", "clear_formatting", "drop_cap"]),
    ("Styles", ["list_styles", "get_style", "modify_style", "delete_style", "import_styles"]),
    ("Lists", ["apply_list", "set_list_level", "remove_list", "restart_numbering", "define_list", "list_lists"]),
    ("Track changes", ["set_track_changes", "list_revisions", "accept_revisions", "reject_revisions", "compare_documents"]),
    ("Comments", ["add_comment", "list_comments", "reply_comment", "resolve_comment", "edit_comment", "delete_comment"]),
    ("Tables", ["insert_table", "read_table", "set_cell", "table_rows", "table_columns", "merge_cells", "split_cell", "format_table", "sort_table", "convert_table", "delete_table"]),
    ("Insert", ["insert_picture", "list_pictures", "format_picture", "delete_picture", "insert_text_box", "insert_break", "insert_hyperlink", "list_hyperlinks", "remove_hyperlinks", "add_bookmark",
                "list_bookmarks", "delete_bookmark", "insert_note", "list_notes", "delete_note", "insert_field", "list_fields",
                "update_fields", "unlink_fields", "insert_toc", "mark_index_entry", "insert_index", "insert_equation", "insert_caption", "insert_cross_reference", "list_reference_targets", "add_content_control", "list_content_controls", "set_content_control"]),
    ("Mailings", ["mail_merge", "document_variables"]),
    ("Page layout", ["get_page_setup", "set_page_setup", "set_header_footer", "remove_header_footer", "read_header_footer", "set_watermark", "theme"]),
    ("File", ["get_properties", "set_properties", "get_settings", "set_settings", "protect_document", "unprotect_document"]),
    ("Expert", ["validate_document", "inspect_document", "get_xml", "set_paragraph_xml"]),
    ("Seeing the page", ["render_pages", "export_pdf"]),
    ("Finding tools", ["help", "run_tool"]),
]


def type_of(p):
    t = p.get("type", "any")
    if "enum" in p:
        return " \\| ".join("`%s`" % e for e in p["enum"])
    if t == "array":
        it = p.get("items", {})
        return "array of " + (("array of " + it["items"].get("type", "")) if it.get("type") == "array" else it.get("type", "any"))
    return t


def esc(s):
    return s.replace("|", "\\|").replace("\n", " ")


def main():
    tools = json.loads(subprocess.run([sys.argv[1], "--list-tools"], capture_output=True, text=True, check=True).stdout)
    by = {t["name"]: t for t in tools}
    seen = set()
    out = ["# Tool reference", "",
           "Generated from `docx-mcp --list-tools` by `tools/gen_tool_docs.py`. %d tools." % len(tools), "",
           "Every tool that edits takes `doc` (a handle or a file path), and most take `track` (record as a tracked "
           "change) and `author`. Paragraphs are addressed by the `id` that `read_document` returns.", ""]
    out.append("## Contents")
    out.append("")
    for g, names in GROUPS:
        out.append("- [%s](#%s): %s" % (g, g.lower().replace(" ", "-"), ", ".join("`%s`" % n for n in names if n in by)))
    out.append("")
    for g, names in GROUPS:
        out.append("## " + g)
        out.append("")
        for n in names:
            if n not in by:
                continue
            seen.add(n)
            t = by[n]
            out.append("### `%s`" % n)
            out.append("")
            if t.get("description"):
                out.append(t["description"])
                out.append("")
            props = t["inputSchema"].get("properties", {})
            req = set(t["inputSchema"].get("required", []))
            if props:
                out.append("| Argument | Type | Required | Description |")
                out.append("|---|---|---|---|")
                for k, p in props.items():
                    out.append("| `%s` | %s | %s | %s |" % (k, esc(type_of(p)), "yes" if k in req else "", esc(p.get("description", ""))))
                out.append("")
    missing = [n for n in by if n not in seen]
    if missing:
        raise SystemExit("tools missing from GROUPS: %s" % missing)
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "tools.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write("\n".join(out) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
