#!/usr/bin/env python3
"""Run the Open XML SDK oracle (oxval) on outputs and their source documents
and report errors the outputs have that the source didn't.

usage: oracle_regress.py SRC_DIR OUT_DIR [OUT_DIR...]
Outputs are matched to sources by file name. Exit 1 when anything is new.
"""
import os
import re
import subprocess
import sys
from collections import Counter

OXVAL = os.environ.get("OXVAL", "/tmp/oxval-bin/oxval")


def run(files):
    errs = {}
    for i in range(0, len(files), 200):
        out = subprocess.run([OXVAL, *files[i:i + 200]], capture_output=True, text=True).stdout
        cur = None
        for line in out.splitlines():
            m = re.match(r"(\S.*\.docx): \d+ error", line)
            if m:
                cur = m[1]
                errs[cur] = Counter()
                continue
            m = re.match(r"\s+\[(\w+)\] (\S+) (\S+) :: (.*)", line)
            if m and cur:
                # key on part + message shape; paths shift with edits
                msg = re.sub(r"'[^']*'", "''", m[4])
                errs[cur][(m[2], msg.strip())] += 1
    return errs


def main():
    src, outs = sys.argv[1], sys.argv[2:]
    srcs = {}
    for root, _, fs in os.walk(src):
        for f in fs:
            if f.endswith(".docx"):
                srcs.setdefault(f, os.path.join(root, f))
    outfiles = []
    for d in outs:
        for root, _, fs in os.walk(d):
            outfiles += [os.path.join(root, f) for f in fs if f.endswith(".docx")]
    base = run(sorted(srcs.values()))
    got = run(sorted(outfiles))
    bad = 0
    for f in sorted(got):
        b = base.get(srcs.get(os.path.basename(f), ""), Counter())
        new = got[f] - b
        if new:
            bad += 1
            print(f)
            for (part, msg), n in new.items():
                print("   %dx %s :: %s" % (n, part, msg[:220]))
    print("%d outputs, %d with new oracle errors" % (len(got), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
