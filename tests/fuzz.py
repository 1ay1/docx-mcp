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


class Fuzz:
    def __init__(self, c, rnd, doc):
        self.c, self.r, self.doc = c, rnd, doc
        self.log = []

    def call(self, tool, **a):
        name = tool
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
        ]
        return r.choice(ops)()

    def table_op(self, track):
        r = self.r
        r0, _ = self.call("read_document", limit=1)
        self.log.pop()
        tb, err = self.call("read_table", table=1)
        if err:
            return None, err
        rows = max(1, len(tb["rows"]))
        cols = max(1, tb["columns"])
        op = r.randint(0, 6)
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
        f = Fuzz(c, rnd, doc)
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
