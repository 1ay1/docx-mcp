#!/usr/bin/env python3
"""Discoverability: can a model find the right tool when its client shows only
some of them?

1. help: for every everyday task, `help {topic}` must rank the right tool in
   its first three.
2. Ranked clients: replays agentty's selector (16 MCP tools per request,
   scored on words in name/description, ties in catalog order) and checks the
   right tool, or help + run_tool, is visible for each task. A broad question
   ("what can you do with word docs") must see help.

usage: discover.py <docx-mcp binary>"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import Client  # noqa: E402

# (what a user says, the tool that does it)
TASKS = [
    ("open a word document", "open_document"),
    ("create a new blank document", "create_document"),
    ("save the document", "save_document"),
    ("read the whole document", "read_document"),
    ("get the text as markdown", "get_text"),
    ("show the headings", "get_outline"),
    ("search for a phrase", "find_text"),
    ("word count", "get_statistics"),
    ("insert text into a paragraph", "insert_text"),
    ("replace a sentence", "replace_text"),
    ("delete some words", "delete_text"),
    ("find and replace everywhere", "find_replace"),
    ("add a new paragraph", "insert_paragraph"),
    ("delete a paragraph", "delete_paragraph"),
    ("split a paragraph in two", "split_paragraph"),
    ("move a paragraph", "move_paragraphs"),
    ("make text bold", "format_text"),
    ("change the font size", "format_text"),
    ("center a paragraph", "format_paragraph"),
    ("make it a heading", "apply_style"),
    ("create a custom style", "modify_style"),
    ("make a bulleted list", "apply_list"),
    ("number the headings 1, 1.1, 1.1.1", "define_list"),
    ("restart numbering", "restart_numbering"),
    ("insert a table", "insert_table"),
    ("change a cell's text", "set_cell"),
    ("add a row to a table", "table_rows"),
    ("merge table cells", "merge_cells"),
    ("split a table cell", "split_cell"),
    ("shade a table", "format_table"),
    ("insert a picture", "insert_picture"),
    ("wrap text around an image", "format_picture"),
    ("resize a picture", "format_picture"),
    ("insert a page break", "insert_break"),
    ("add a hyperlink", "insert_hyperlink"),
    ("add a footnote", "insert_note"),
    ("insert a table of contents", "insert_toc"),
    ("insert an equation", "insert_equation"),
    ("add a bookmark", "add_bookmark"),
    ("add a checkbox", "add_content_control"),
    ("set page margins", "set_page_setup"),
    ("landscape orientation", "set_page_setup"),
    ("add a header with page numbers", "set_header_footer"),
    ("turn on track changes", "set_track_changes"),
    ("accept all tracked changes", "accept_revisions"),
    ("reject a tracked change", "reject_revisions"),
    ("compare two versions", "compare_documents"),
    ("add a comment", "add_comment"),
    ("reply to a comment", "reply_comment"),
    ("resolve a comment", "resolve_comment"),
    ("set the document title", "set_properties"),
    ("protect the document with a password", "protect_document"),
    ("undo the last change", "undo"),
    ("update fields", "update_fields"),
    ("mail merge letters from a csv of addresses", "mail_merge"),
    ("paste this html into the document", "insert_html"),
    ("draw an arrow shape", "insert_shape"),
    ("add a bar chart of these numbers", "insert_chart"),
    ("cite a book and add a bibliography", "insert_citation"),
    ("add a figure caption", "insert_caption"),
    ("insert a cross-reference to a figure", "insert_cross_reference"),
]
BROAD = ["what can you do with ms word docs?", "help me edit a docx", "work on my Word file"]


def agentty_select(tools, query, k=16):
    """agentty's select_wire_tools_from, for one MCP server named docx."""
    q = re.sub(r"[^a-z0-9]", " ", query.lower())
    terms = [t for t in q.split() if len(t) >= 2]
    scored = []
    for i, t in enumerate(tools):
        name = "mcp__docx__" + t["name"]
        hay = (name + " docx " + t.get("description", "")).lower()
        s = 0
        for term in terms:
            if term in name:
                s += 8
            if term in "docx":
                s += 5
            if term in hay:
                s += 2
        scored.append((s, i, t["name"]))
    if len(scored) <= k:
        return {n for _, _, n in scored}
    scored.sort(key=lambda x: (-x[0], x[1]))
    return {n for _, _, n in scored[:k]}


def main():
    exe = sys.argv[1]
    fails = []
    c = Client(exe)
    c.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "d", "version": "1"}})
    for task, want in TASKS:
        txt = c.rpc("tools/call", {"name": "help", "arguments": {"topic": task}})["result"]["content"][0]["text"]
        # tool names as listed: "name(" for search hits, "name:" for an exact tool
        got = re.findall(r"^\s*([a-z_]+)[(:]", txt, re.M)[:3]
        if want not in got:
            fails.append("help %r -> %s, want %s" % (task, got, want))
    tools = json.loads(subprocess.run([exe, "--list-tools"], capture_output=True, text=True).stdout)
    names = {t["name"] for t in tools}
    for need in ("help", "run_tool"):
        if need not in names:
            fails.append("%s missing from the catalog" % need)
    for t in tools:
        if len(t.get("description", "")) < 15:
            fails.append("%s has no real description" % t["name"])
    direct = reach = 0
    for task, want in TASKS:
        seen = agentty_select(tools, task)
        if want in seen:
            direct += 1
        if want in seen or {"help", "run_tool"} <= seen:
            reach += 1
        else:
            fails.append("ranked client: %r can't reach %s (sees %s)" % (task, want, sorted(seen)[:6]))
    for q in BROAD:
        seen = agentty_select(tools, q)
        if "help" not in seen:
            fails.append("ranked client: broad question %r doesn't see help" % q)
    print("help ranks the right tool for %d/%d tasks" % (len(TASKS) - sum(f.startswith("help ") for f in fails), len(TASKS)))
    print("ranked client: right tool visible %d/%d, reachable %d/%d" % (direct, len(TASKS), reach, len(TASKS)))
    for f in fails:
        print("FAIL", f)
    print("discover", "FAILED" if fails else "passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
