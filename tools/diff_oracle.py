#!/usr/bin/env python3
"""Compare our validator (probe_validate) with the Open XML SDK oracle (oxval)
on the same files: prints (file, part, path) locations only one side flags."""
import re
import subprocess
import sys

files = [l.strip() for l in open(sys.argv[1]) if l.strip()]
ours = subprocess.run(["./build/tests/probe_validate", *files], capture_output=True, text=True).stdout
oracle = subprocess.run(["/tmp/oxval-bin/oxval", *files], capture_output=True, text=True).stdout


def norm(path):
    return re.sub(r"\[\d+\]$", "", path)


a = set()
for line in ours.splitlines():
    m = re.match(r"(\S+) (\S+) (\S+) :: (.*)", line)
    if m:
        a.add((m[1], m[2], m[3]))
b = set()
cur = None
for line in oracle.splitlines():
    m = re.match(r"(\S+): \d+ error", line)
    if m:
        cur = m[1]
        continue
    m = re.match(r"\s+\[\w+\] (\S+) (\S+) :: (.*)", line)
    if m and cur:
        b.add((cur, m[1], m[2]))


def close(x, s):
    # the two sides may report the same fault on parent vs child
    def rel(p, q):
        return p == q or p.startswith(q + "/") or q.startswith(p + "/")
    return any(y[0] == x[0] and y[1] == x[1] and rel(x[2], y[2]) for y in s)


only_ours = sorted(x for x in a if not close(x, b))
only_oracle = sorted(x for x in b if not close(x, a))
print("ours=%d oracle=%d only-ours=%d only-oracle=%d" % (len(a), len(b), len(only_ours), len(only_oracle)))
for x in only_ours[:40]:
    print("OURS  ", *x)
for x in only_oracle[:40]:
    print("ORACLE", *x)
