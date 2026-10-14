#!/usr/bin/env python3
"""Replay a fuzz crash log (crash.json from tests/fuzz.py) against a binary,
then shrink it to the fewest calls that still kill the server.

usage: replay_crash.py <docx-mcp binary> <crash.json>"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(exe, src, log):
    """True if the server dies while replaying log on a fresh copy of src."""
    work = tempfile.mkdtemp()
    path = os.path.join(work, "doc.docx")
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    msgs = [{"jsonrpc": "2.0", "id": 0, "method": "initialize",
             "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "r", "version": "1"}}}]
    if src:
        shutil.copy(os.path.join(HERE, "corpus", "oxpt", src), path)
        msgs.append({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "open_document", "arguments": {"path": path}}})
    else:
        msgs.append({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "create_document", "arguments": {"path": path}}})
    for i, (name, a) in enumerate(log):
        a = dict(a, doc="doc1")
        msgs.append({"jsonrpc": "2.0", "id": i + 2, "method": "tools/call", "params": {"name": name, "arguments": a}})
    try:
        p.communicate("".join(json.dumps(m) + "\n" for m in msgs), timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
        return False
    shutil.rmtree(work, ignore_errors=True)
    return p.returncode != 0


def main():
    exe, crash = sys.argv[1], sys.argv[2]
    data = json.load(open(crash))
    src, log = data["src"], [tuple(x) for x in data["log"]]
    if not run(exe, src, log):
        print("does not reproduce")
        return 1
    # drop calls one at a time while it still crashes
    i = 0
    while i < len(log):
        trial = log[:i] + log[i + 1:]
        if run(exe, src, trial):
            log = trial
        else:
            i += 1
    print("minimal (%d calls) on %s:" % (len(log), src))
    for name, a in log:
        print("  ", name, json.dumps({k: v for k, v in a.items() if k != "doc"}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
