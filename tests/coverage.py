#!/usr/bin/env python3
"""Coverage: call every tool, tracked and untracked, on a set of real
documents, then save edited / accepted / rejected variants. Every output must
reopen, and with the oracle built (tools/oxval) must add no Open XML SDK errors
over its source.

usage: coverage.py <docx-mcp binary> <workdir> [docx...]"""
import base64
import json
import os
import shutil
import subprocess
import sys
import zlib
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import Client  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus", "oxpt")
DEFAULT = ["HC001-5DayTourPlanTemplate.docx", "DA001-TemplateDocument.docx", "RP/RP001-Tracked-Revisions-01.docx",
           "HC006-Test-01.docx", "DB007-Spec.docx", "WC/WC002-Unmodified.docx", "HC002-Hebrew-01.docx"]


def png():
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + b"\xff\x00\x00" * 4 for _ in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class Run:
    def __init__(self, c):
        self.c = c
        self.fail = []
        self.calls = 0
        self.used = set()

    def call(self, _tool, ok_errors=(), **a):
        name = _tool
        """Call a tool. Tool errors are fine when they read as a clean message;
        protocol errors and crashes are failures."""
        self.calls += 1
        self.used.add(name)
        r = self.c.rpc("tools/call", {"name": name, "arguments": a})
        if "error" in r:
            self.fail.append("%s: rpc error %s" % (name, r["error"]))
            return None
        res = r["result"]
        if res.get("isError"):
            msg = res["content"][0]["text"]
            bad = any(s in msg for s in ("std::", "basic_string", "vector", "out_of_range", "stoi", "json.exception",
                                         "internal error", "nullptr", "what()"))
            if bad or not any(s in msg for s in ok_errors) and ok_errors != "*":
                if bad or ok_errors != "*":
                    self.fail.append("%s(%s): %s" % (name, json.dumps(a)[:160], msg[:300]))
            return None
        sc = res.get("structuredContent", {})
        return sc.get("result", sc) if list(sc.keys()) == ["result"] else sc


def new_id(r):
    if isinstance(r, dict) and r.get("inserted"):
        return r["inserted"][0]["id"]
    return None


def paras(R, doc):
    r = R.call("read_document", doc=doc, limit=400)
    return r["paragraphs"] if r else []


def body_text_paras(R, doc, n=6):
    return [p for p in paras(R, doc) if len(p.get("text", "")) > 12 and "in" not in p][:n]


def exercise(R, doc, track, work, tag):
    """Every tool once (or more) against document `doc`."""
    t = {"track": track}
    R.call("set_track_changes", doc=doc, on=track)
    ps = body_text_paras(R, doc, 8)
    if len(ps) < 4:
        R.call("insert_paragraph", doc=doc, text="Seed paragraph one with enough text to edit.", **t)
        R.call("insert_paragraph", doc=doc, text="Seed paragraph two with enough text to edit.", **t)
        R.call("insert_paragraph", doc=doc, text="Seed paragraph three with enough text to edit.", **t)
        R.call("insert_paragraph", doc=doc, text="Seed paragraph four with enough text to edit.", **t)
        ps = body_text_paras(R, doc, 8)
    if len(ps) < 4:
        R.fail.append("%s: no paragraphs to edit" % tag)
        return
    p0, p1, p2, p3 = (p["id"] for p in ps[:4])
    words = [w for w in ps[0]["text"].split() if len(w) > 2]
    word = words[-1] if words else ps[0]["text"][-3:]

    # reading
    R.call("get_text", doc=doc)
    R.call("get_text", doc=doc, format="text", view="original")
    R.call("get_outline", doc=doc)
    R.call("get_paragraph", doc=doc, paragraph=p0)
    R.call("read_document", doc=doc, runs=True, limit=5)
    R.call("find_text", doc=doc, text=word)
    R.call("find_text", doc=doc, text="[a-z]+", regex=True, limit=3)
    R.call("get_statistics", doc=doc)
    R.call("list_open_documents")

    # text
    R.call("insert_text", doc=doc, paragraph=p0, after=word, text="+", **t)
    R.call("insert_text", doc=doc, paragraph=p0, position=2, text=" inserted\ttext ", format={"bold": True}, **t)
    R.call("replace_text", doc=doc, paragraph=p1, start=0, end=3, text="Rep", **t)
    R.call("delete_text", doc=doc, paragraph=p1, start=4, end=7, **t)
    R.call("find_replace", doc=doc, find="the", replace="THE", limit=3, **t)
    R.call("find_replace", doc=doc, find=r"(\w)(\w)ing", replace="$2$1ING", regex=True, limit=2, **t)
    R.call("format_text", doc=doc, paragraph=p2, start=0, end=5,
           format={"italic": True, "underline": "double", "color": "red", "highlight": "yellow", "size": 13,
                   "font": "Arial", "strike": True}, **t)
    R.call("format_text", doc=doc, paragraph=p2, start=6, end=8, format={"superscript": True}, **t)
    R.call("format_paragraph", doc=doc, paragraphs=[p2, p3],
           format={"alignment": "center", "indent_left": 18, "space_after": 6, "line_spacing": 1.5,
                   "keep_with_next": True}, **t)
    R.call("apply_style", doc=doc, paragraphs=[p3], style="Heading 2", **t)
    R.call("apply_style", doc=doc, paragraphs=[p2], style="Strong", match=word, ok_errors="*", **t)
    R.call("clear_formatting", doc=doc, paragraphs=[p3], paragraph_too=False, **t)

    # paragraphs
    np = R.call("insert_paragraph", doc=doc, paragraph=p1, text="Brand new paragraph\nwith a line break",
                style="Quote", paragraph_format={"alignment": "right"}, format={"bold": True}, **t)
    np_id = new_id(np)
    np2 = R.call("insert_paragraph", doc=doc, paragraph=p1, where="before", text="Before paragraph to split", **t)
    np2_id = new_id(np2)
    if np2_id:
        R.call("split_paragraph", doc=doc, paragraph=np2_id, position=6, **t)
        R.call("merge_paragraphs", doc=doc, paragraph=np2_id, **t)
    if np_id:
        R.call("move_paragraphs", doc=doc, paragraphs=[np_id], target=p3, where="after", **t)
    np3 = R.call("insert_paragraph", doc=doc, paragraph=p3, text="Doomed paragraph", **t)
    if new_id(np3):
        R.call("delete_paragraph", doc=doc, paragraphs=[new_id(np3)], **t)
    R.call("insert_markdown", doc=doc, paragraph=p3, markdown=(
        "## Imported\n\nSome **bold**, *italic*, `code` and a [link](https://example.com).\n\n"
        "- one\n  - nested\n- two\n\n1. first\n2. second\n\n> quoted\n\n| A | B |\n|---|--:|\n| x | **y** |\n\n"
        "```\ncode block\n```\n\n---\n"), **t)

    # styles
    R.call("list_styles", doc=doc)
    R.call("list_styles", doc=doc, include_builtin=True, type="character")
    R.call("get_style", doc=doc, style="Heading 1")
    sname = "Cov Style " + tag
    R.call("modify_style", doc=doc, style=sname, type="paragraph", based_on="Normal", next="Normal",
           run={"bold": True, "color": "336699"}, paragraph={"space_before": 12}, quick_style=True)
    R.call("modify_style", doc=doc, style="Heading 1", run={"color": "1F3864"})
    R.call("apply_style", doc=doc, paragraphs=[p0], style=sname, **t)
    cs = "Cov Char " + tag
    R.call("modify_style", doc=doc, style=cs, type="character", run={"italic": True})
    R.call("modify_style", doc=doc, style="Cov Del " + tag, type="paragraph")
    R.call("delete_style", doc=doc, style="Cov Del " + tag)

    # lists
    lp = []
    for i in range(3):
        r = R.call("insert_paragraph", doc=doc, paragraph=p3, text="List item %d" % i, **t)
        if new_id(r):
            lp.append(new_id(r))
    if lp:
        R.call("apply_list", doc=doc, paragraphs=lp, kind="number", start=3, **t)
        R.call("set_list_level", doc=doc, paragraphs=lp[1:2], level=1, **t)
        R.call("restart_numbering", doc=doc, paragraph=lp[2], start=7)
        R.call("apply_list", doc=doc, paragraphs=lp[2:], kind="bullet", bullet="▪", **t)
        R.call("remove_list", doc=doc, paragraphs=lp[:1], **t)
        R.call("apply_list", doc=doc, paragraphs=lp[:1], kind="upperRoman", text="%1)", **t)

    # comments
    c1 = R.call("add_comment", doc=doc, paragraph=p0, start=0, end=4, text="First comment", initials="CV")
    c2 = R.call("add_comment", doc=doc, paragraph=p1, end_paragraph=p2, text="Spanning comment")
    R.call("add_comment", doc=doc, paragraph=p2, match=word, text="match comment", ok_errors="*")
    cid = c1.get("comment_id") if isinstance(c1, dict) else None
    if cid:
        R.call("reply_comment", doc=doc, comment_id=cid, text="A reply")
        R.call("edit_comment", doc=doc, comment_id=cid, text="Edited comment")
        R.call("resolve_comment", doc=doc, comment_id=cid)
    if isinstance(c2, dict) and c2.get("comment_id"):
        R.call("delete_comment", doc=doc, comment_id=c2["comment_id"])
    R.call("list_comments", doc=doc, include_resolved=True)

    # tables
    tb = R.call("insert_table", doc=doc, paragraph=p1, rows=[["H1", "H2", "H3"], ["a", "b", "c"], ["d", "e", "f"]],
                header_row=True, repeat_header=True, style="Table Grid", alignment="center", widths=[100, 120, 80], **t)
    ti = tb.get("table") if isinstance(tb, dict) else None
    if ti:
        R.call("read_table", doc=doc, table=ti)
        R.call("set_cell", doc=doc, table=ti, row=2, col=2, text="cell text", format={"bold": True}, **t)
        R.call("table_rows", doc=doc, table=ti, action="insert_below", row=2, cells=["x", "y", "z"], **t)
        R.call("table_rows", doc=doc, table=ti, action="insert_above", row=1, **t)
        R.call("table_rows", doc=doc, table=ti, action="delete", row=3, **t)
        R.call("table_columns", doc=doc, table=ti, action="insert_right", col=2, **t)
        R.call("table_columns", doc=doc, table=ti, action="insert_left", col=1, **t)
        R.call("table_columns", doc=doc, table=ti, action="delete", col=1, **t)
        R.call("merge_cells", doc=doc, table=ti, from_row=2, from_col=1, to_row=3, to_col=2)
        R.call("split_cell", doc=doc, table=ti, row=2, col=1)
        R.call("split_cell", doc=doc, table=ti, row=1, col=1, cols=2)
        R.call("format_table", doc=doc, table=ti,
               format={"borders": "all", "width": 400, "alignment": "right", "cell_margins": 5,
                       "banded_rows": True, "repeat_header": True}, **t)
        R.call("set_cell", doc=doc, table=ti, row=1, col=1, format={"shading": "EEEEEE", "vertical_align": "center",
                                                                   "alignment": "center"}, **t)
    tb2 = R.call("insert_table", doc=doc, paragraph=p2, rows=[["del", "me"]], **t)
    if isinstance(tb2, dict) and tb2.get("table"):
        R.call("delete_table", doc=doc, table=tb2["table"], **t)

    # objects
    R.call("insert_picture", doc=doc, paragraph=p0, position=1, data_base64=base64.b64encode(png()).decode(),
           width=40, alt_text="red square", **t)
    R.call("insert_picture", doc=doc, paragraph=p1, data_base64=base64.b64encode(png()).decode(), width=30,
           layout={"wrap": "square", "horizontal": "right", "vertical_from": "paragraph", "y": 6}, **t)
    R.call("insert_picture", doc=doc, paragraph=p2, data_base64=base64.b64encode(png()).decode(), width=30,
           layout="behind", **t)
    R.call("list_pictures", doc=doc)
    tp = R.call("insert_paragraph", doc=doc, paragraph=p2, text="K\tV\nb\t2\na\t10", **t)
    tids = [x["id"] for x in (tp or {}).get("inserted", [])] if isinstance(tp, dict) else []
    if len(tids) == 3:
        ct = R.call("convert_table", doc=doc, to="table", paragraphs=tids, style="Grid Table 4 Accent 1", **t)
        if isinstance(ct, dict) and ct.get("table"):
            if track:
                R.call("set_track_changes", doc=doc, on=False)
            R.call("sort_table", doc=doc, table=ct["table"], col=2, type="number", descending=True)
            if track:
                R.call("set_track_changes", doc=doc, on=True)
            R.call("convert_table", doc=doc, to="text", table=ct["table"], separator="comma", **t)
    R.call("insert_text_box", doc=doc, paragraph=p1, text="Callout\nsecond line", fill="DEEAF6", border="1F4E79",
           shape="rounded", format={"bold": True}, layout={"wrap": "square", "horizontal": "right"}, width=150, **t)
    R.call("insert_text_box", doc=doc, paragraph=p2, text="No border", border=False, **t)
    R.call("format_picture", doc=doc, picture=1, width=50, layout="tight", alt_text="moved")
    R.call("format_picture", doc=doc, picture=2, layout="inline")
    R.call("delete_picture", doc=doc, picture=3, **t)
    R.call("insert_break", doc=doc, paragraph=p1, position=2, type="line", **t)
    R.call("insert_break", doc=doc, paragraph=p2, position=3, type="column", **t)
    R.call("insert_break", doc=doc, paragraph=p3, type="page", **t)
    R.call("insert_hyperlink", doc=doc, paragraph=p0, position=0, url="https://example.com/a?b=1&c=2",
           text="link", tooltip="tip", **t)
    R.call("insert_hyperlink", doc=doc, paragraph=p1, match="Rep", url="#cov_bm", ok_errors="*", **t)
    R.call("list_hyperlinks", doc=doc)
    R.call("remove_hyperlinks", doc=doc, target_contains="example.com")
    R.call("add_bookmark", doc=doc, paragraph=p1, start=0, end=3, name="cov_bm")
    R.call("add_bookmark", doc=doc, paragraph=p2, name="cov_bm2")
    R.call("list_bookmarks", doc=doc, include_hidden=True)
    R.call("delete_bookmark", doc=doc, name="cov_bm2")
    fn = R.call("insert_note", doc=doc, paragraph=p0, position=3, text="A footnote", **t)
    R.call("insert_note", doc=doc, paragraph=p1, kind="endnote", text="An endnote", **t)
    notes = R.call("list_notes", doc=doc) or []
    nr = R.call("insert_note", doc=doc, paragraph=p2, text="Doomed note", **t)
    notes = R.call("list_notes", doc=doc) or []
    notes = [n for n in notes if isinstance(n, dict)]
    doomed = [n for n in notes if "Doomed" in n.get("text", "")] or notes
    if doomed:
        R.call("delete_note", doc=doc, kind=doomed[0].get("kind", "footnote"), id=str(doomed[0]["id"]), **t)
    else:
        R.fail.append("%s: no notes to delete; insert_note returned %s, list_notes %s" % (tag, json.dumps(nr), json.dumps(notes)))
    R.call("insert_field", doc=doc, paragraph=p1, position=1, code="PAGE", result="1", **t)
    R.call("insert_field", doc=doc, paragraph=p1, code="DATE \\@ \"yyyy-MM-dd\"", **t)
    R.call("insert_field", doc=doc, paragraph=p2, code="REF cov_bm \\h", **t)
    R.call("list_fields", doc=doc)
    cap = R.call("insert_caption", doc=doc, paragraph=p0, text="A caption", **t)
    R.call("insert_caption", doc=doc, table=1, label="Table", text="Table caption", ok_errors="*", **t)
    R.call("list_reference_targets", doc=doc)
    if isinstance(cap, dict) and cap.get("id"):
        for show in ("label_number", "text", "number", "page"):
            R.call("insert_cross_reference", doc=doc, paragraph=p3, target=cap["id"], show=show, **t)
    R.call("insert_toc", doc=doc, paragraph=p0, title="Contents", from_level=1, to_level=3, **t)
    R.call("insert_toc", doc=doc, paragraph=p0, captions="Figure", title="Figures", **t)
    R.call("mark_index_entry", doc=doc, paragraph=p2, term="zeta", position=0, **t)
    R.call("mark_index_entry", doc=doc, paragraph=p3, term="alpha", sub="beta: gamma", position=1, **t)
    R.call("insert_index", doc=doc, paragraph=p3, columns=2, **t)
    R.call("update_fields", doc=doc)
    ml = R.call("define_list", doc=doc, preset="outline", paragraphs=[p1, p2], level=1, name="Cov outline", **t)
    R.call("modify_style", doc=doc, name="Cov Table", type="table", table={"borders": {"style": "single", "color": "9BC2E6"}},
           conditional={"header_row": {"run": {"bold": True}, "cell": {"shading": "1F4E79"}}, "banded_rows": {"cell": {"shading": "DDEBF7"}},
                        "total_row": {"bold": True}})
    R.call("get_style", doc=doc, style="CovTable")
    R.call("theme", doc=doc)
    src_tag = (R.call("add_source", doc=doc, type="book", title="Cov Book", authors=["Smith, Jane"], year="2020", publisher="Pub") or {}).get("tag", "Smith20")
    R.call("add_source", doc=doc, type="article", title="Cov Article", authors=["Ann Lee", "Bo Ng"], year=2021, journal="J", volume="1", pages="1-2", tag="Lee21")
    R.call("add_source", doc=doc, list=True)
    R.call("insert_citation", doc=doc, paragraph=p1, position=2, source=src_tag, pages="5", **t)
    R.call("insert_bibliography", doc=doc, paragraph=p3, style="MLA", **t)
    R.call("insert_bibliography", doc=doc, style="IEEE")
    R.call("add_source", doc=doc, type="website", title="Gone", tag="Gone1")
    R.call("add_source", doc=doc, remove="Gone1")
    R.call("insert_chart", doc=doc, paragraph=p1, type="column", title="Cov chart", categories=["Q1", "Q2"],
           series=[{"name": "A", "values": [1, 2]}, {"name": "B", "values": [3, None], "color": "C00000"}], data_labels=True, **t)
    R.call("insert_chart", doc=doc, paragraph=p2, type="pie", series=[{"values": [30, 70]}], categories=["x", "y"], width=200, height=150, **t)
    R.call("list_charts", doc=doc)
    R.call("set_chart_data", doc=doc, chart=1, series=[{"name": "C", "values": [5, 6]}], grouping="stacked")
    R.call("insert_shape", doc=doc, paragraph=p2, shape="ellipse", text="Start", width=100, height=50, **t)
    R.call("insert_shape", doc=doc, paragraph=p2, shape="line", width=40, end_arrow="triangle", border={"color": "404040", "size": 2},
           layout={"wrap": "in_front", "x": 100, "y": 20})
    R.call("insert_shape", doc=doc, paragraph=p3, shape="decision", text="OK?", fill="FFC000", rotation=10, border=False, **t)
    R.call("insert_html", doc=doc, paragraph=p3, html="<h3>From HTML</h3><p>Some <b>bold</b> and <a href='https://example.com'>a link</a>.</p>"
           "<ul><li>one</li><li>two<ol><li>deep</li></ol></li></ul><table><tr><th>A</th><th>B</th></tr><tr><td colspan=2>wide</td></tr></table>", **t)
    R.call("theme", doc=doc, preset="Blue II", heading_font="Georgia", colors={"accent2": "C00000", "hyperlink": "0000FF"}, name="Cov")
    R.call("theme", doc=doc, fonts_preset="Garamond", colors_preset="Paper")
    R.call("import_styles", doc=doc, styles=["Heading 1", "ListParagraph"], **{"from": os.path.join(CORPUS, "DB007-Spec.docx")})
    R.call("import_styles", doc=doc, overwrite=False, defaults=True, **{"from": os.path.join(CORPUS, "DB007-Spec.docx")})
    R.call("define_list", doc=doc, preset="numbered", levels=[{"format": "upperLetter", "text": "%1)"}, {"text": "%1.%2", "restart_after": False}])
    R.call("define_list", doc=doc, preset="headings")
    lists = R.call("list_lists", doc=doc) or {}
    if isinstance(ml, dict) and "num_id" in ml:
        R.call("apply_list", doc=doc, paragraphs=[p3], num_id=ml["num_id"], level=2, **t)
    R.call("update_fields", doc=doc, paragraph=p1)
    R.call("document_variables", doc=doc, set={"Version": "2.1", "Gone": None})
    R.call("document_variables", doc=doc)
    R.call("insert_field", doc=doc, paragraph=p2, code="DOCVARIABLE Version", **t)
    R.call("insert_field", doc=doc, paragraph=p2, code="IF { = 2 * 3 } > 5 \"big\" \"small\"", **t)
    R.call("insert_field", doc=doc, paragraph=p3, code="MERGEFIELD Name \\* Upper", **t)
    R.call("mail_merge", doc=doc)
    R.call("mail_merge", doc=doc, records=[{"Name": "Ann"}, {"Name": "Bob"}], output_dir=os.path.join(work, "merge_" + tag), name_field="Name")
    R.call("mail_merge", doc=doc, records=[{"Name": "Cy"}], output=os.path.join(work, "edited", "merged_%s.docx" % tag))
    R.call("unlink_fields", doc=doc, kinds=["DOCVARIABLE"], **{})
    R.call("insert_equation", doc=doc, paragraph=p2, position=1, equation="x^2 + y_1 = \\sqrt(z)/2", **t)
    R.call("insert_equation", doc=doc, paragraph=p3, equation="\\sum_(i=1)^n i", display=True, **t)
    cc = []
    for ty, extra in (("richText", {}), ("text", {"placeholder": "Type"}), ("checkbox", {"checked": True}),
                      ("dropDown", {"items": ["One", "Two"]}), ("comboBox", {"items": ["A", "B"]}),
                      ("date", {"date_format": "yyyy-MM-dd"}), ("picture", {})):
        r = R.call("add_content_control", doc=doc, paragraph=p3, type=ty, tag="cov_" + ty, title=ty,
                   lock_delete=ty == "text", **extra)
        if isinstance(r, dict):
            cc.append((ty, r))
    R.call("list_content_controls", doc=doc)
    for ty, v in (("text", "filled"), ("checkbox", False), ("dropDown", "Two"), ("comboBox", "custom"),
                  ("date", "2024-05-06"), ("richText", "rich")):
        R.call("set_content_control", doc=doc, control="cov_" + ty, value=v)
    R.call("bind_content_control", doc=doc, control="cov_text", property="title")
    R.call("bind_content_control", doc=doc, control="cov_richText", data="client/name")
    R.call("set_content_control", doc=doc, control="cov_richText", value="Acme")
    R.call("set_properties", doc=doc, core={"title": "Bound title"}, company="Acme Inc")
    R.call("bind_content_control", doc=doc, control="cov_richText")
    rs = R.call("repeating_section", doc=doc, first=p0, tag="cov_rep", add=1)
    if rs:
        R.call("repeating_section", doc=doc, control="cov_rep", add=2)

    # page, headers
    R.call("get_page_setup", doc=doc)
    R.call("set_page_setup", doc=doc, setup={"orientation": "landscape", "margins": {"top": 54, "left": 60},
                                             "columns": 2, "paper": "a4", "page_numbers": {"start": 3}}, **t)
    R.call("insert_break", doc=doc, paragraph=p2, type="section_next_page", **t)
    R.call("set_page_setup", doc=doc, section=2, setup={"orientation": "portrait", "title_page": True}, **t)
    R.call("set_page_setup", doc=doc, section=1, setup={"page_borders": {"style": "double", "color": "1F4E79"},
                                                        "page_color": "FDF6E3", "column_line": True,
                                                        "column_widths": ["3in", "2in"]}, **t)
    R.call("set_header_footer", doc=doc, kind="header", text="Header {PAGE} of {NUMPAGES}", alignment="center")
    R.call("set_header_footer", doc=doc, kind="footer", which="first", text="First footer")
    R.call("set_header_footer", doc=doc, kind="header", which="even", text="Even header")
    R.call("read_header_footer", doc=doc, kind="header")
    R.call("remove_header_footer", doc=doc, kind="header", which="even")
    R.call("set_watermark", doc=doc, text="DRAFT")
    R.call("set_watermark", doc=doc, text="CONFIDENTIAL", layout="horizontal", color="red")
    R.call("set_watermark", doc=doc, picture="data:image/png;base64," + base64.b64encode(png()).decode(), washout=True, scale=500)
    if not track:
        R.call("set_watermark", doc=doc, remove=True)

    # doc-level
    R.call("inspect_document", doc=doc)
    if not track:
        R.call("inspect_document", doc=doc, remove=["hidden_text", "personal_info", "custom_properties"])
    dc = R.call("insert_paragraph", doc=doc, paragraph=p3, text="Drop cap paragraph with some words.", **t)
    if new_id(dc):
        R.call("drop_cap", doc=doc, paragraph=new_id(dc), lines=2, **t)
        if not track:
            R.call("drop_cap", doc=doc, paragraph=new_id(dc), kind="none")
    R.call("get_properties", doc=doc)
    R.call("set_properties", doc=doc, core={"title": "Coverage <&> test", "subject": "s", "keywords": "a, b",
                                            "description": "d"},
           custom={"Client": "ACME", "Count": 3, "Flag": True, "Ratio": 1.5})
    R.call("get_settings", doc=doc)
    R.call("set_settings", doc=doc, settings={"update_fields_on_open": True, "default_tab_stop": 36, "zoom": 120})
    R.call("list_revisions", doc=doc)
    R.call("validate_document", doc=doc)
    R.call("get_xml", doc=doc, paragraph=p0, max_chars=2000)
    R.call("get_xml", doc=doc, part="word/styles.xml", max_chars=500)
    R.call("set_paragraph_xml", doc=doc, paragraph=p3,
           xml='<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">Raw XML paragraph </w:t></w:r></w:p>')
    R.call("protect_document", doc=doc, mode="trackedChanges", password="secret")
    R.call("unprotect_document", doc=doc, password="wrong", ok_errors=("password",))
    R.call("unprotect_document", doc=doc, password="secret")
    R.call("protect_document", doc=doc, mode="readOnly")
    R.call("unprotect_document", doc=doc)
    R.call("undo", doc=doc)
    R.call("undo", doc=doc, steps=2)
    R.call("help")
    R.call("help", topic="tables")
    R.call("help", topic="insert_table")
    R.call("run_tool", tool="get_statistics", arguments={"doc": doc})
    R.call("run_tool", tool="insert_paragraph", arguments={"doc": doc, "text": "via run_tool", **t})
    R.call("run_tool", tool="no_such_tool", arguments={}, ok_errors=("unknown tool",))
    R.call("batch", doc=doc, steps=[{"tool": "insert_paragraph", "arguments": {"text": "batched one", **t}},
                                   {"tool": "insert_markdown", "markdown": "- batched\n- list"},
                                   {"tool": "get_statistics"}])
    R.call("batch", doc=doc, steps=[{"tool": "insert_paragraph", "arguments": {"text": "rolled back"}},
                                   {"tool": "format_text", "arguments": {"paragraph": "ZZZZZZZZ", "format": {"bold": True}}}],
           ok_errors=("Nothing was changed",))
    if os.environ.get("COVER_RENDER"):  # needs LibreOffice; slow, so opt-in
        R.call("render_pages", doc=doc, page=1, dpi=40)
        R.call("export_pdf", doc=doc, path=os.path.join(work, "out_%s.pdf" % tag))


def main():
    exe, work = sys.argv[1], sys.argv[2]
    docs = sys.argv[3:] or [os.path.join(CORPUS, d) for d in DEFAULT]
    shutil.rmtree(work, ignore_errors=True)
    for sub in ("edited", "accepted", "rejected"):
        os.makedirs(os.path.join(work, sub))
    c = Client(exe)
    c.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "cov", "version": "1"}})
    c.notify("notifications/initialized")
    R = Run(c)
    srcs = list(docs) + [None]
    for i, src in enumerate(srcs):
        name = os.path.basename(src) if src else "blank.docx"
        for track in (False, True):
            tag = "%d%s" % (i, "t" if track else "u")
            if src:
                path = os.path.join(work, "in_" + tag + "_" + name)
                shutil.copy(src, path)
                r = R.call("open_document", path=path)
            else:
                path = os.path.join(work, "in_" + tag + "_" + name)
                r = R.call("create_document", path=path, title="Blank", paper="a4")
            if not r:
                continue
            doc = r["doc"]
            exercise(R, doc, track, work, tag)
            out = os.path.join(work, "edited", tag + "_" + name)
            R.call("save_document", doc=doc, path=out)
            R.call("close_document", doc=doc)
            for verb in ("accept", "reject"):
                r = R.call("open_document", path=out)
                if not r:
                    continue
                R.call(verb + "_revisions", doc=r["doc"])
                left = R.call("list_revisions", doc=r["doc"])
                if left and left.get("count"):
                    R.fail.append("%s: %d revisions left after %s_revisions" % (tag, left["count"], verb))
                R.call("save_document", doc=r["doc"], path=os.path.join(work, verb + "ed", tag + "_" + name))
                R.call("close_document", doc=r["doc"])
    # compare the edited file with its source
    edited = sorted(os.listdir(os.path.join(work, "edited")))
    if edited:
        R.call("compare_documents", original=os.path.join(work, "accepted", edited[0]),
               revised=os.path.join(work, "edited", edited[0]), output=os.path.join(work, "edited", "cmp_" + edited[0]))
        if os.environ.get("COVER_RENDER"):
            R.call("compare_documents", original=os.path.join(work, "accepted", edited[0]),
                   revised=os.path.join(work, "edited", edited[0]), output=os.path.join(work, "cmp2.docx"), pages=True, dpi=40)
        # File > New from a template, and Save As .dotx
        tpl = os.path.join(work, "cov_template.dotx")
        r = R.call("open_document", path=os.path.join(work, "edited", edited[0]))
        if r:
            R.call("save_document", doc=r["doc"], path=tpl)
            R.call("close_document", doc=r["doc"])
            r = R.call("create_document", path=os.path.join(work, "edited", "from_template.docx"), template=tpl)
            if r:
                R.call("save_document", doc=r["doc"])
                R.call("close_document", doc=r["doc"])
    tools = json.loads(subprocess.run([exe, "--list-tools"], capture_output=True, text=True).stdout)
    optional = set() if os.environ.get("COVER_RENDER") else {"render_pages", "export_pdf"}
    missing = sorted({t["name"] for t in tools} - R.used - optional)
    if missing:
        R.fail.append("tools never called: " + ", ".join(missing))
    print("%d calls, %d tools" % (R.calls, len(R.used)))
    for f in R.fail:
        print("FAIL", f)
    rc = 1 if R.fail else 0
    if os.path.exists(os.environ.get("OXVAL", "/tmp/oxval-bin/oxval")):
        src_dir = os.path.join(work, "src")
        os.makedirs(src_dir, exist_ok=True)
        # outputs are named <tag>_<source name>; give the oracle script the same names
        for src in docs:
            for f in edited:
                if f.endswith("_" + os.path.basename(src)):
                    shutil.copy(src, os.path.join(src_dir, f))
        r = subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "oracle_regress.py"), src_dir,
                            os.path.join(work, "edited"), os.path.join(work, "accepted"),
                            os.path.join(work, "rejected")])
        rc |= r.returncode
    print("coverage", "FAILED" if rc else "passed")
    sys.exit(rc)


if __name__ == "__main__":
    main()
