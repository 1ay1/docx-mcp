#!/usr/bin/env python3
"""Fuzz: random sequences of tool calls on real documents. After every call
the document must still validate (no schema issues we introduced) and the
server must answer; at the end, save + accept + reject must all validate too.
Tool errors are fine when they read as a clean message.

usage: fuzz.py <docx-mcp binary> <workdir> [seconds] [seed]"""
import json
import os
import random
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import Client  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus", "oxpt")
DOCS = ["HC001-5DayTourPlanTemplate.docx", "DA001-TemplateDocument.docx", "RP/RP001-Tracked-Revisions-01.docx",
        "HC006-Test-01.docx", "WC/WC002-Unmodified.docx", "HC002-Hebrew-01.docx", "RP/RP013-Deleted-Math-Control-Char.docx",
        "HC032-Named-Color.docx", "DB007-Spec.docx", None]

# messages that mean an internal fault, not a clean refusal
LEAKS = ("std::", "basic_string", "vector::", "out_of_range", "stoi", "stol", "stod", "json.exception", "nullptr",
         "invalid_argument", "bad_alloc", "unknown error", "internal")

WORDS = ["alpha", "beta", "gamma", "the", "and", "Section", "über", "日本", "עברית", "x", "  ", "\t", "a\nb", ""]


def _png():
    import base64, struct, zlib
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + b"\x00\x80\xff" * 2 for _ in range(2))
    return base64.b64encode(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)) +
                            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")).decode()


PNG = _png()


class Fuzz:
    def __init__(self, c, rnd, doc, path=None):
        self.c, self.r, self.doc = c, rnd, doc
        self.path = path
        self.log = []

    def call(self, _tool, **a):
        name = _tool
        a = dict(doc=self.doc, **a)
        self.log.append((name, a))
        r = self.c.rpc("tools/call", {"name": name, "arguments": a})
        if "error" in r:
            return None, "rpc: %s" % r["error"]
        res = r["result"]
        if res.get("isError"):
            return None, res["content"][0]["text"]
        sc = res.get("structuredContent", {})
        return (sc.get("result", sc) if list(sc.keys()) == ["result"] else sc), None

    def paras(self):
        r, _ = self.call("read_document", limit=500)
        self.log.pop()
        return r["paragraphs"] if r else []

    def word(self):
        return self.r.choice(WORDS)

    def text(self):
        return " ".join(self.word() for _ in range(self.r.randint(1, 4)))

    def step(self):
        r = self.r
        ps = self.paras()
        if not ps:
            return self.call("insert_paragraph", text="seed")
        p = r.choice(ps)
        pid = p["id"] if r.random() < 0.9 else str(p["n"])
        txt = p.get("text", "")
        n = len(txt)
        s = r.randint(0, n)
        e = r.randint(s, n)
        sub = txt[s:e] if e > s else None
        track = {"track": r.random() < 0.6}
        ops = [
            lambda: self.call("insert_text", paragraph=pid, position=s, text=self.text(), **track),
            lambda: self.call("insert_text", paragraph=pid, text=self.text(), format={"bold": True, "size": r.choice([9, "14", "12pt"])}, **track),
            lambda: self.call("delete_text", paragraph=pid, start=s, end=e, **track),
            lambda: self.call("replace_text", paragraph=pid, start=s, end=e, text=self.text(), **track),
            lambda: self.call("replace_text", paragraph=pid, match=sub, text=self.text(), **track) if sub else self.call("get_text"),
            lambda: self.call("format_text", paragraph=pid, start=s, end=e,
                              format=r.choice([{"italic": True}, {"underline": "double"}, {"color": "red"}, {"highlight": "yellow"},
                                               {"strike": True}, {"style": "Strong"}, {"bold": None}, {"font": "Arial"}]), **track),
            lambda: self.call("format_paragraph", paragraphs=[pid],
                              format=r.choice([{"alignment": "center"}, {"space_after": "6pt"}, {"indent_left": "1cm"},
                                               {"line_spacing": "double"}, {"keep_with_next": True}, {"style": "Heading 2"}]), **track),
            lambda: self.call("insert_paragraph", paragraph=pid, where=r.choice(["after", "before"]), text=self.text(),
                              style=r.choice(["", "Quote", "Heading 1", "List Paragraph"]) or None, **track),
            lambda: self.call("delete_paragraph", paragraphs=[pid], **track),
            lambda: self.call("split_paragraph", paragraph=pid, position=s, **track),
            lambda: self.call("merge_paragraphs", paragraph=pid, **track),
            lambda: self.call("move_paragraphs", paragraphs=[pid], target=r.choice(ps)["id"], where=r.choice(["after", "before"]), **track),
            lambda: self.call("apply_list", paragraphs=[pid], kind=r.choice(["bullet", "number", "lowerLetter"]), **track),
            lambda: self.call("remove_list", paragraphs=[pid], **track),
            lambda: self.call("add_comment", paragraph=pid, start=s, end=max(e, s + 1) if n else 0, text=self.text()),
            lambda: self.call("insert_table", paragraph=pid, rows=[[self.word() for _ in range(r.randint(1, 3))] for _ in range(r.randint(1, 3))], **track),
            lambda: self.table_op(track),
            lambda: self.call("insert_break", paragraph=pid, position=s, type=r.choice(["line", "page", "column"]), **track),
            lambda: self.call("insert_hyperlink", paragraph=pid, position=s, url="https://example.com/?a=1&b=2", text=self.word() or "link", **track),
            lambda: self.call("insert_note", paragraph=pid, position=s, kind=r.choice(["footnote", "endnote"]), text=self.text(), **track),
            lambda: self.call("insert_field", paragraph=pid, position=s, code=r.choice(["PAGE", "NUMPAGES", "DATE"]), **track),
            lambda: self.call("insert_equation", paragraph=pid, position=s, equation=r.choice(["x^2", "a/b", "\\sqrt(x)"]), **track),
            lambda: self.call("add_bookmark", paragraph=pid, start=s, end=e, name="bm%d" % r.randint(0, 9)),
            lambda: self.call("find_replace", find=r.choice(["the", "a", "e"]), replace=self.word(), limit=3, **track),
            lambda: self.call("clear_formatting", paragraphs=[pid], **track),
            lambda: self.call("accept_revisions"),
            lambda: self.call("reject_revisions"),
            lambda: self.call("undo"),
            lambda: self.call("set_track_changes", on=r.random() < 0.5),
            lambda: self.extra(pid, s, e, track),
            lambda: self.extra(pid, s, e, track),
            lambda: self.extra(pid, s, e, track),
        ]
        return r.choice(ops)()

    def first(self, _tool, key, **a):
        """A random item from a list tool, or None."""
        v, _ = self.call(_tool, **a)
        self.log.pop()
        if isinstance(v, dict):
            v = next((x for x in v.values() if isinstance(x, list)), [])
        v = [x for x in (v or []) if isinstance(x, dict) and x.get(key) is not None]
        return self.r.choice(v)[key] if v else None

    def extra(self, pid, s, e, track):
        r = self.r
        k = r.randint(0, 52)
        if k == 50:
            return self.call("insert_field", paragraph=pid, position=s, code=r.choice([
                "= 2 + 3 * 4 \\# \"0.00\"", "IF { = 1 + 1 } = 2 \"yes\" \"no\"", "MERGEFIELD Name", "DOCVARIABLE V",
                "SEQ Figure \\* roman", "DOCPROPERTY Title \\* Upper", "STYLEREF 1", "QUOTE \"q\" \\* Caps",
                "= SUM(ABOVE)", "COMPARE 3 > 2", "REF bm1 \\h", "DATE \\@ \"dddd, MMMM d\""]), **track)
        if k == 51:
            if r.random() < 0.5:
                return self.call("unlink_fields", kinds=r.choice([["MERGEFIELD"], ["IF", "="], ["DATE"]]))
            return self.call("document_variables", set={"V": self.word() or "v", "W": None})
        if k == 52:
            out = self.path.replace("run", "out").replace(".docx", "_merge%d.docx" % r.randint(0, 99))
            return self.call("mail_merge", records=[{"Name": self.word() or "n"}, {"Name": "b"}], output=out)
        if k == 40:
            return self.call("insert_markdown", paragraph=pid, markdown=r.choice([
                "## H\n\n**b** *i* `c` [l](https://example.com)", "- a\n  - b\n1. x\n2. y", "| A | B |\n|---|--:|\n| 1 | **2** |",
                "> q\n\n```\ncode\n```\n\n---", "plain " + self.word()]), **track)
        if k == 41:
            return self.call("insert_text_box", paragraph=pid, position=s, text=self.text() or "box",
                             fill=r.choice(["DEEAF6", "yellow"]), border=r.choice(["1F4E79", False]), **track)
        if k == 42:
            return self.call("set_watermark", text=r.choice(["DRAFT", "A & <B>"]), remove=r.random() < 0.3)
        if k == 43:
            return self.call("set_page_setup", setup=r.choice([{"page_borders": "single"}, {"page_color": "FDF6E3"},
                                                             {"columns": 2, "column_line": True}, {"column_widths": ["3in", "2in"]},
                                                             {"page_borders": None, "page_color": None}]), **track)
        if k == 44:
            return self.call("drop_cap", paragraph=pid, kind=r.choice(["drop", "margin", "none"]), **track)
        if k == 45:
            return self.call("mark_index_entry", paragraph=pid, term=self.word(), position=s, **track)
        if k == 46:
            return self.call(r.choice(["insert_index", "insert_toc"]), paragraph=pid,
                             **({"captions": "Figure"} if r.random() < 0.5 else {}), **track)
        if k == 47:
            return self.call("sort_table", table=1, col=r.randint(1, 3), type=r.choice(["text", "number", "date"]), track=False)
        if k == 48:
            if r.random() < 0.5:
                return self.call("convert_table", to="text", table=1, **track)
            return self.call("convert_table", to="table", paragraphs=[pid], separator=r.choice(["tab", "comma", " "]), **track)
        if k == 49:
            if r.random() < 0.2:
                return self.call("inspect_document", remove=[r.choice(["comments", "hidden_text", "personal_info", "watermark", "custom_xml"])])
            return self.call("batch", steps=[{"tool": "insert_text", "paragraph": pid, "text": self.word(), **track},
                                             {"tool": "format_text", "paragraph": pid, "format": {"bold": True}, **track}])
        if k == 37:
            return self.call("insert_picture", paragraph=pid, position=s, data_base64=PNG, width=r.choice([20, 80]),
                             layout=r.choice(["inline", "square", "tight", "through", "top_bottom", "behind", "in_front",
                                              {"wrap": "square", "horizontal": "center", "vertical_from": "page", "y": 72}]), **track)
        if k == 38:
            return self.call("format_picture", picture=r.randint(1, 3), width=r.choice([0, 40]),
                             layout=r.choice(["inline", "square", "behind", {"wrap": "in_front", "x": 10, "y": 10}]))
        if k == 39:
            return self.call("delete_picture", picture=r.randint(1, 3), **track)
        if k == 34:
            return self.call("insert_caption", paragraph=pid, label=r.choice(["Figure", "Table", "Chart"]), text=self.text() or "c", **track)
        if k == 35:
            tg = self.first("list_reference_targets", "target")
            return self.call("insert_cross_reference", paragraph=pid, position=s, target=tg or pid,
                             show=r.choice(["text", "number", "label_number", "page", "above_below"]), **track)
        if k == 36:
            return self.call("list_reference_targets")
        if k == 0:
            return self.call(r.choice(["get_outline", "get_statistics", "list_styles", "list_revisions", "list_comments",
                                        "list_hyperlinks", "list_bookmarks", "list_notes", "list_fields",
                                        "list_content_controls", "get_page_setup", "get_properties", "get_settings",
                                        "list_open_documents"]))
        if k == 1:
            return self.call("get_paragraph", paragraph=pid)
        if k == 2:
            return self.call("find_text", text=r.choice(["the", "a", "[A-Z]\\w+"]), regex=r.random() < 0.3, whole_word=r.random() < 0.3)
        if k == 3:
            return self.call("apply_style", paragraphs=[pid], style=r.choice(["Heading 1", "Title", "Quote", "Normal", "Strong"]), **track)
        if k == 4:
            return self.call("get_style", style=r.choice(["Heading 2", "Normal", "Fuzz Style", "Caption"]))
        if k == 5:
            return self.call("modify_style", style=r.choice(["Fuzz Style", "Heading 1", "Normal"]), type="paragraph",
                             run={"bold": r.random() < 0.5, "color": "336699"}, paragraph={"space_after": r.randint(0, 24)})
        if k == 6:
            return self.call("delete_style", style="Fuzz Style")
        if k == 7:
            return self.call("set_list_level", paragraphs=[pid], level=r.randint(0, 8), **track)
        if k == 8:
            return self.call("restart_numbering", paragraph=pid, start=r.randint(1, 9))
        if k == 9:
            cid = self.first("list_comments", "id", include_resolved=True)
            if cid is None:
                return self.call("list_comments")
            op = r.randint(0, 3)
            if op == 0:
                return self.call("reply_comment", comment_id=str(cid), text=self.text() or "re")
            if op == 1:
                return self.call("resolve_comment", comment_id=str(cid), resolved=r.random() < 0.7)
            if op == 2:
                return self.call("edit_comment", comment_id=str(cid), text=self.text() or "edit")
            return self.call("delete_comment", comment_id=str(cid))
        if k == 10:
            return self.call("format_table", table=1, format=r.choice([{"borders": "all"}, {"alignment": "center"}, {"width": "100%"},
                                                                         {"cell_margins": 4}, {"banded_rows": True}, {"style": "Table Grid"}]), **track)
        if k == 11:
            return self.call("insert_picture", paragraph=pid, position=s, data_base64=PNG, width=r.choice([20, 60, 900]), **track)
        if k == 12:
            return self.call("remove_hyperlinks", target_contains="example")
        if k == 13:
            name = self.first("list_bookmarks", "name")
            return self.call("delete_bookmark", name=name) if name else self.call("list_bookmarks")
        if k == 14:
            notes, _ = self.call("list_notes")
            self.log.pop()
            notes = notes if isinstance(notes, list) else (notes or {}).get("notes", [])
            if not notes:
                return self.call("list_notes")
            n = r.choice(notes)
            return self.call("delete_note", kind=n.get("kind", "footnote"), id=str(n.get("id")), **track)
        if k == 15:
            return self.call("update_fields")
        if k == 16:
            return self.call("insert_toc", paragraph=pid, from_level=1, to_level=r.randint(1, 4), **track)
        if k == 17:
            ty = r.choice(["richText", "text", "checkbox", "dropDown", "comboBox", "date", "picture"])
            extra = {"items": ["One", "Two"]} if ty in ("dropDown", "comboBox") else {}
            return self.call("add_content_control", paragraph=pid, start=s, end=e, type=ty, tag="t%d" % r.randint(0, 3), **extra)
        if k == 18:
            return self.call("set_content_control", control="t%d" % r.randint(0, 3), value=r.choice(["x", True, "Two", "2024-01-02"]))
        if k == 19:
            return self.call("set_page_setup", setup=r.choice([{"orientation": "landscape"}, {"paper": "a4"}, {"margins": {"top": "1in"}},
                                                                {"columns": r.randint(1, 3)}, {"title_page": True}]), **track)
        if k == 20:
            return self.call("set_header_footer", kind=r.choice(["header", "footer"]), which=r.choice(["default", "first", "even"]),
                             text=r.choice(["Page {PAGE}", self.text() or "h"]))
        if k == 21:
            return self.call("remove_header_footer", kind=r.choice(["header", "footer"]), which=r.choice(["default", "first", "even"]))
        if k == 22:
            return self.call("read_header_footer", kind=r.choice(["header", "footer"]))
        if k == 23:
            return self.call("set_properties", core={"title": self.text()}, custom={"K": r.choice([1, "v", True, 2.5])})
        if k == 24:
            return self.call("set_settings", settings=r.choice([{"zoom": 120}, {"update_fields_on_open": True}, {"mirror_margins": True},
                                                                {"default_tab_stop": "0.5in"}]))
        if k == 25:
            return self.call("protect_document", mode=r.choice(["readOnly", "comments", "trackedChanges", "forms"]),
                             password=r.choice(["", "pw"]))
        if k == 26:
            return self.call("unprotect_document", password=r.choice(["", "pw"]))
        if k == 27:
            return self.call("get_xml", paragraph=pid, max_chars=500)
        if k == 28:
            return self.call("set_paragraph_xml", paragraph=pid,
                             xml='<w:p><w:r><w:t xml:space="preserve">raw %s</w:t></w:r></w:p>' % r.randint(0, 99))
        if k == 29:
            return self.call("compare_documents", original=self.path, revised=self.path)
        if k == 30:
            # a header paragraph: edit inside another story
            hf, _ = self.call("read_header_footer", kind="header")
            self.log.pop()
            ps = (hf or {}).get("paragraphs") or []
            if not ps:
                return self.call("set_header_footer", kind="header", text="Head")
            ref = "%s#%s" % (hf["part"], ps[0]["id"]) if hf.get("part") else ps[0]["id"]
            return self.call("insert_text", paragraph=ref, text=" hf", **track)
        if k == 31:
            return self.call("list_revisions")
        if k == 32:
            rid = self.first("list_revisions", "id")
            verb = r.choice(["accept_revisions", "reject_revisions"])
            return self.call(verb, ids=[str(rid)]) if rid is not None else self.call(verb)
        author = self.first("list_revisions", "author")
        return self.call(r.choice(["accept_revisions", "reject_revisions"]), by_author=author or "nobody")

    def table_op(self, track):
        r = self.r
        r0, _ = self.call("read_document", limit=1)
        self.log.pop()
        tb, err = self.call("read_table", table=1)
        if err:
            return None, err
        rows = max(1, len(tb["rows"]))
        cols = max(1, tb["columns"])
        op = r.randint(0, 7)
        if op == 7:
            return self.call("split_cell", table=1, row=r.randint(1, rows), col=r.randint(1, cols), cols=r.randint(1, 3))
        if op == 0:
            return self.call("table_rows", table=1, action=r.choice(["insert_above", "insert_below"]), row=r.randint(1, rows), **track)
        if op == 1:
            return self.call("table_rows", table=1, action="delete", row=r.randint(1, rows), **track)
        if op == 2:
            return self.call("table_columns", table=1, action=r.choice(["insert_left", "insert_right"]), col=r.randint(1, cols), **track)
        if op == 3:
            return self.call("table_columns", table=1, action="delete", col=r.randint(1, cols), **track)
        if op == 4:
            return self.call("set_cell", table=1, row=r.randint(1, rows), col=r.randint(1, cols), text=self.text(), **track)
        if op == 5:
            r1, c1 = r.randint(1, rows), r.randint(1, cols)
            return self.call("merge_cells", table=1, from_row=r1, from_col=c1, to_row=r.randint(r1, rows), to_col=r.randint(c1, cols))
        return self.call("delete_table", table=1, **track)


def issues(c, doc):
    r = c.rpc("tools/call", {"name": "validate_document", "arguments": {"doc": doc}})
    sc = r["result"].get("structuredContent", {})
    return sc.get("issues", [])


def main():
    exe, work = sys.argv[1], sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else int(time.time())
    print("seed", seed)
    rnd = random.Random(seed)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    c = Client(exe)
    c.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "fuzz", "version": "1"}})
    end = time.time() + seconds
    runs = steps = 0
    fails = []
    while time.time() < end and len(fails) < 5:
        src = rnd.choice(DOCS)
        path = os.path.join(work, "run%d.docx" % runs)
        if src:
            shutil.copy(os.path.join(CORPUS, src), path)
            r = c.call("open_document", path=path)
        else:
            r = c.call("create_document", path=path)
        doc = r["doc"]
        base = len(issues(c, doc))
        f = Fuzz(c, rnd, doc, path)
        for _ in range(rnd.randint(5, 40)):
            steps += 1
            if os.environ.get("FUZZ_KEEP"):
                # the document before this step, to replay a failure in a unit test
                c.rpc("tools/call", {"name": "save_document", "arguments": {"doc": doc, "path": os.path.join(work, "last.docx"), "validate": False}})
            try:
                _, err = f.step()
            except SystemExit:
                fails.append((src, f.log, "server died"))
                with open(os.path.join(work, "crash.json"), "w") as fp:
                    json.dump({"src": src, "log": f.log}, fp, ensure_ascii=False, indent=0)
                print("FAIL server died on", src, json.dumps(f.log[-1], ensure_ascii=False)[:300])
                print("   replay: python3 tests/replay_crash.py <exe>", os.path.join(work, "crash.json"))
                return 1
            if err and any(x in err for x in LEAKS):
                fails.append((src, f.log, err))
                break
            iss = issues(c, doc)
            if len(iss) > base:
                fails.append((src, f.log, "schema after %s: %s" % (f.log[-1][0], json.dumps(iss[:2], ensure_ascii=False))))
                break
        else:
            out = os.path.join(work, "out%d.docx" % runs)
            _, err = f.call("save_document", path=out)
            if err:
                fails.append((src, f.log, "save: " + err))
            c.call("close_document", doc=doc)
            doc = None
            for verb in ("accept", "reject"):
                if err:
                    break
                d2 = c.call("open_document", path=out)["doc"]
                c.call(verb + "_revisions", doc=d2)
                iss = issues(c, d2)
                if iss:
                    fails.append((src, f.log, "%s: %s" % (verb, json.dumps(iss[:2], ensure_ascii=False))))
                c.call("close_document", doc=d2)
        if doc:
            c.call("close_document", doc=doc)
        runs += 1
    for src, log, why in fails:
        print("FAIL on", src, ":", why[:400])
        for name, a in log[-3:]:
            print("   ", name, json.dumps({k: v for k, v in a.items() if k != "doc"}, ensure_ascii=False)[:200])
    print("%d runs, %d steps, %d failures (seed %d)" % (runs, steps, len(fails), seed))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
