#!/usr/bin/env python3
"""Sloppy-model test: send the kinds of arguments real models send (wrong
types, camelCase names, strings for lists, JSON-as-string objects, enum case,
handle variants, quoted paths, ~ paths) and check the server still does the
right thing. Also protocol robustness: batches, garbage, huge ids, unknown
methods, notifications, BOM, CRLF.

usage: sloppy.py <docx-mcp binary> <workdir>"""
import json
import os
import shutil
import subprocess
import sys


def main():
    exe, work = sys.argv[1], sys.argv[2]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    nid = [0]

    def send_raw(b):
        p.stdin.write(b)
        p.stdin.flush()

    def read():
        line = p.stdout.readline()
        if not line:
            raise SystemExit("server died: " + p.stderr.read().decode(errors="replace"))
        return json.loads(line)

    def rpc(method, params=None, raw_id=None):
        nid[0] += 1
        rid = raw_id if raw_id is not None else nid[0]
        m = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            m["params"] = params
        send_raw((json.dumps(m) + "\n").encode())
        return read()

    def call(name, **args):
        r = rpc("tools/call", {"name": name, "arguments": args})
        res = r.get("result", {})
        if "error" in r or res.get("isError"):
            raise AssertionError(f"{name}{args}: {r.get('error') or res['content'][0]['text']}")
        return res.get("structuredContent", {})

    def call_err(name, **args):
        r = rpc("tools/call", {"name": name, "arguments": args})
        res = r.get("result", {})
        return bool(res.get("isError")), (res.get("content") or [{"text": ""}])[0]["text"]

    ok = [0]

    def check(c, msg):
        if not c:
            raise AssertionError(msg)
        ok[0] += 1
        print("  ok", msg)

    # protocol: BOM + CRLF on the first frame
    send_raw(b"\xef\xbb\xbf" + json.dumps({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
        "protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "sloppy", "version": "1"}}}).encode() + b"\r\n")
    init = read()
    check(init.get("result", {}).get("serverInfo", {}).get("name") == "docx-mcp", "initialize with BOM + CRLF")
    check(init["result"]["protocolVersion"] == "2025-03-26", "negotiates the client's version (2025-03-26)")
    send_raw(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
    send_raw(b"this is not json\n")
    check(read().get("error", {}).get("code") == -32700, "garbage -> parse error, server alive")
    send_raw(b'[{"jsonrpc":"2.0","id":"a","method":"ping"},{"jsonrpc":"2.0","id":"b","method":"tools/list"}]\n')
    b = read()
    check(isinstance(b, list) and len(b) == 2 and {x["id"] for x in b} == {"a", "b"}, "batch -> array reply")
    check(rpc("no/such/method").get("error", {}).get("code") == -32601, "unknown method -> -32601")
    check("result" in rpc("ping", raw_id=2**53 - 1), "large integer id")
    check("result" in rpc("ping", raw_id="string-id-é"), "string id with unicode")

    # sloppy arguments
    path = os.path.join(work, "Sloppy Doc é.docx")  # spaces + non-ASCII in the name
    h = call("create_document", path='"' + path + '"', author="Model")["doc"]
    check(h.startswith("doc"), "create with a quoted path containing spaces and é")
    call("insert_paragraph", doc=h, text="First line\nSecond line\nThird line", where="AFTER")
    r = call("read_document", doc=h.upper(), limit="10", runs="yes")
    check(r["total_paragraphs"] == 3, "handle in upper case, limit as string, runs as 'yes' (and no stray blank line)")
    ids = [x["id"] for x in r["paragraphs"]]
    call("format_paragraph", doc=h, paragraphs=ids[0], format='{"alignment":"Center","space_after":"12"}')
    check(True, "single id for a list + format as a JSON string")
    call("format_text", doc=h, paragraphId=ids[1], match="Second", format={"bold": "true"})
    check(True, "camelCase argument name (paragraphId)")
    call("apply_list", doc=h, paragraphs=json.dumps(ids[:2]), kind="Bullet")
    check(True, "list as a JSON string, enum in wrong case")
    call("insert_table", doc=h, paragraph=ids[-1], rows=[{"Item": "Setup", "Price": 500}, {"Item": "Support", "Price": 1000}])
    t = call("read_table", doc=h, table="1")
    check(t["rows"][0] == ["Item", "Price"] and t["rows"][2][1] == "1000", "table rows as objects, numbers as cells")
    call("set_track_changes", doc=h, on="on")
    call("replace_text", document=h, paragraph=ids[2], old_text="Third", new_text="3rd")
    revs = call("list_revisions", doc=h)
    check(revs["count"] >= 2, "aliases: document / old_text / new_text, track on via 'on'")
    e, msg = call_err("insert_text", doc=h, paragraph=ids[0], text="x", bogus_flag=1)
    check(e and "Valid arguments" in msg, "unknown argument on an edit -> refused, lists valid ones")
    e, msg = call_err("get_paragraph", doc=h, paragraph=ids[0], xml_hint=True)
    check(not e and "ignored 'xml_hint'" in msg, "unknown argument on a read -> answered, noted")
    e, msg = call_err("insert_text", doc=h, paragraph=ids[0])
    check(e and "missing required argument 'text'" in msg, "missing required -> names it")
    e, msg = call_err("read_document", doc="doc99")
    check(e and "Open documents" in msg, "bad handle -> lists open documents")
    e, msg = call_err("insert_text", doc=h, paragraph="ZZZZ", text="x")
    check(e and "not found" in msg, "bad paragraph -> helpful error")
    call("save_document", doc=h)
    check(os.path.exists(path), "saved file with a non-ASCII name")
    # reopen by path variants
    h2 = call("open_document", path=path)["doc"]
    check(h2 == h, "opening an open file returns the same handle")
    home = os.path.expanduser("~")
    if path.startswith(home):
        h3 = call("open_document", path="~" + path[len(home):])["doc"]
        check(h3 == h, "~ path resolves to the same document")
    call("close_document", doc=h)

    # Windows-style paths (only meaningful when the server is a Windows build;
    # under wine, Z: maps to /). Backslashes, forward slashes and mixed.
    if os.environ.get("DOCX_WINDOWS_PATHS"):
        unix = os.path.join(work, "win paths.docx")
        winp = "Z:" + unix.replace("/", "\\")
        hw = call("create_document", path=winp)["doc"]
        call("insert_text", doc=hw, paragraph="1", text="windows path")
        call("save_document", doc=hw)
        check(os.path.exists(unix), "save to a C:\\-style backslash path")
        hw2 = call("open_document", path="z:/" + unix.lstrip("/"))["doc"]
        check(hw2 == hw, "forward-slash + lower-case drive is the same document")

    p.stdin.close()
    p.wait(timeout=10)
    err = p.stderr.read().decode(errors="replace")
    check(p.returncode == 0, "clean exit on stdin close")
    oxval = "/tmp/oxval-bin/oxval"
    if os.path.exists(oxval):
        r = subprocess.run([oxval, path], capture_output=True, text=True)
        check(r.returncode == 0, "Open XML SDK: 0 errors (" + r.stdout.strip() + ")")
    print(f"sloppy passed ({ok[0]} checks)")


if __name__ == "__main__":
    main()
