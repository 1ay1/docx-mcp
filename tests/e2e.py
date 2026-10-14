#!/usr/bin/env python3
"""End-to-end test: drive the docx-mcp binary over stdio JSON-RPC like an MCP
client and run a realistic drafting + review workflow. Checks the outputs
with the Open XML SDK oracle (if built) and LibreOffice (if present).

usage: e2e.py <docx-mcp binary> <workdir>"""
import json
import os
import shutil
import subprocess
import sys


class Client:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe, "--author", "Alice Author"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.id = 0

    def rpc(self, method, params=None):
        self.id += 1
        msg = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params is not None:
            msg["params"] = params
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise SystemExit("server closed")
            r = json.loads(line)
            if r.get("id") == self.id:
                return r

    def notify(self, method):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.p.stdin.flush()

    def call(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        if "error" in r:
            raise AssertionError(f"{name}: rpc error {r['error']}")
        res = r["result"]
        if res.get("isError"):
            raise AssertionError(f"{name}: {res['content'][0]['text']}")
        sc = res.get("structuredContent", {})
        return sc.get("result", sc) if list(sc.keys()) == ["result"] else sc

    def call_err(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        return r["result"].get("isError", False), r["result"]["content"][0]["text"]


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("  ok", msg)


def main():
    exe, work = sys.argv[1], sys.argv[2]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    c = Client(exe)
    init = c.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                "clientInfo": {"name": "e2e", "version": "1"}})
    check("result" in init and init["result"]["serverInfo"]["name"] == "docx-mcp", "initialize")
    c.notify("notifications/initialized")
    tools = c.rpc("tools/list")["result"]["tools"]
    check(len(tools) >= 80, f"tools/list returns {len(tools)} tools")
    names = {t["name"] for t in tools}
    for t in tools:
        assert t["inputSchema"]["type"] == "object", t["name"]
        assert isinstance(t["inputSchema"].get("properties", {}), dict), t["name"] + ": properties must be an object"

    # ── draft a document ──
    path = os.path.join(work, "contract.docx")
    d = c.call("create_document", path=path, author="Alice Author", title="Services Agreement")["doc"]
    r = c.call("read_document", doc=d)
    first = r["paragraphs"][0]["id"]
    c.call("insert_text", doc=d, paragraph=first, text="Services Agreement")
    c.call("apply_style", doc=d, paragraphs=[first], style="Title")
    ins = c.call("insert_paragraph", doc=d, paragraph=first, text="1. Scope\nThe Provider will deliver the services described in Schedule A.\n2. Fees\nThe Client pays 1,000 USD per month within 30 days.")["inserted"]
    check(len(ins) == 4, "insert 4 paragraphs")
    c.call("apply_style", doc=d, paragraphs=[ins[0]["id"], ins[2]["id"]], style="Heading 1")
    c.call("format_text", doc=d, paragraph=ins[3]["id"], match="1,000 USD", format={"bold": True})
    c.call("insert_table", doc=d, paragraph=ins[3]["id"], rows=[["Item", "Price"], ["Setup", "500"], ["Support", "1,000"]], style="Grid Table 4 Accent 1")
    c.call("set_header_footer", doc=d, kind="footer", text="Page {PAGE} of {NUMPAGES}", alignment="center")
    c.call("insert_toc", doc=d, paragraph=ins[0]["id"], title="Contents")
    c.call("set_properties", doc=d, core={"subject": "MSA"}, custom={"Client": "Acme Corp"})
    c.call("save_document", doc=d)
    check(os.path.getsize(path) > 3000, "saved draft")

    # ── review with track changes, as a second person ──
    c.call("set_track_changes", doc=d, on=True, author="Bob Reviewer")
    hits = c.call("find_text", doc=d, text="30 days")["hits"]
    check(len(hits) == 1, "find_text")
    c.call("replace_text", doc=d, paragraph=hits[0]["paragraph"], match="30 days", text="45 days", author="Bob Reviewer")
    c.call("add_comment", doc=d, paragraph=hits[0]["paragraph"], match="45 days", text="Our standard is 45.", author="Bob Reviewer")
    c.call("insert_text", doc=d, paragraph=ins[1]["id"], after="Schedule A", text=" and Schedule B", author="Bob Reviewer")
    c.call("format_paragraph", doc=d, paragraphs=[ins[1]["id"]], format={"alignment": "both"}, author="Bob Reviewer")
    revs = c.call("list_revisions", doc=d)
    check(revs["count"] >= 4, f"{revs['count']} revisions tracked")
    check(all(r["author"] == "Bob Reviewer" for r in revs["revisions"]), "revisions attributed to Bob")
    cm = c.call("list_comments", doc=d)
    check(len(cm) == 1 and cm[0]["on_text"] == "45 days", "comment anchored on the new text")

    # ── author responds ──
    reply = c.call("reply_comment", doc=d, comment_id=cm[0]["id"], text="Agreed.", author="Alice Author")["comment_id"]
    c.call("resolve_comment", doc=d, comment_id=cm[0]["id"])
    cm = c.call("list_comments", doc=d)
    check(any(x.get("reply_to") == cm[0]["id"] for x in cm) and cm[0]["resolved"], "threaded reply + resolved")
    # accept Bob's text edits, reject his formatting change
    fmt_ids = [r["id"] for r in revs["revisions"] if r["type"] == "paragraph_format"]
    c.call("reject_revisions", doc=d, ids=fmt_ids)
    c.call("accept_revisions", doc=d, by_author="Bob Reviewer")
    check(c.call("list_revisions", doc=d)["count"] == 0, "all revisions resolved")
    txt = c.call("get_text", doc=d)["text"]
    check("45 days" in txt and "Schedule A and Schedule B" in txt and "30 days" not in txt, "accepted text")
    v = c.call("validate_document", doc=d)
    check(v["valid"], "schema valid: " + json.dumps(v["issues"])[:300])
    c.call("save_document", doc=d)

    # ── protection and undo ──
    c.call("protect_document", doc=d, mode="trackedChanges", password="s3cret")
    err, msg = c.call_err("unprotect_document", doc=d, password="wrong")
    check(err and "wrong password" in msg, "wrong password refused")
    c.call("unprotect_document", doc=d, password="s3cret")
    before = c.call("get_text", doc=d)["text"]
    c.call("delete_paragraph", doc=d, paragraphs=[ins[1]["id"]], track=False)
    c.call("undo", doc=d)
    check(c.call("get_text", doc=d)["text"] == before, "undo restores")

    # ── compare ──
    v2 = os.path.join(work, "contract_v2.docx")
    c.call("save_document", doc=d, path=v2)
    d2 = c.call("open_document", path=v2)["doc"]
    hits = c.call("find_text", doc=d2, text="Provider")["hits"]
    c.call("find_replace", doc=d2, find="Provider", replace="Supplier", track=False)
    c.call("save_document", doc=d2)
    cmp = c.call("compare_documents", original=path, revised=v2, output=os.path.join(work, "compare.docx"), author="Compare")
    check(cmp["revisions"] >= 2, f"compare found {cmp['revisions']} revisions")

    # ── errors are tool errors, not crashes ──
    err, msg = c.call_err("insert_text", doc=d, paragraph="DEADBEEF", text="x")
    check(err and "not found" in msg, "bad paragraph id -> isError")
    err, msg = c.call_err("format_text", doc=d, paragraph=ins[1]["id"], format={"boldd": True})
    check(err and "unknown run property" in msg, "bad property -> isError")

    c.p.stdin.close()
    c.p.wait(timeout=10)
    outs = [os.path.join(work, f) for f in sorted(os.listdir(work)) if f.endswith(".docx")]
    oxval = "/tmp/oxval-bin/oxval"
    if os.path.exists(oxval):
        r = subprocess.run([oxval, *outs], capture_output=True, text=True)
        print(r.stdout.strip())
        check(r.returncode == 0, "Open XML SDK: 0 errors on all outputs")
    lo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "lo_check.py")
    if shutil.which("soffice"):
        r = subprocess.run([sys.executable, lo, *outs], capture_output=True, text=True, timeout=200)
        print(r.stdout.strip())
        check(r.returncode == 0, "LibreOffice opens all outputs")
    print("e2e passed")


if __name__ == "__main__":
    main()
