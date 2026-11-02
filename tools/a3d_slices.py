#!/usr/bin/env python3
"""Group A3D coefficient-block writes from a vortex_trace timeline into per-slice updates."""
import sys, re
from collections import defaultdict, Counter

def rows(path):
    for l in open(path, errors='replace'):
        f = l.split()
        if len(f) < 5 or f[1] != 'W' or f[2] == '?':
            continue
        try:
            yield float(f[0]), int(f[2], 16), int(f[4], 16)
        except ValueError:
            continue

def find_runs(path, minlen=40):
    """Return list of (t, base, words) for contiguous ascending write runs >= minlen words."""
    runs = []
    cur = None
    for t, r, v in rows(path):
        if cur and r == cur[1] + 4 * len(cur[2]):
            cur[2].append(v)
            continue
        if cur and len(cur[2]) >= minlen:
            runs.append(tuple(cur))
        cur = [t, r, [v]]
    if cur and len(cur[2]) >= minlen:
        runs.append(tuple(cur))
    return runs

if __name__ == '__main__':
    runs = find_runs(sys.argv[1])
    c = Counter((b, len(w)) for t, b, w in runs)
    for (b, n), k in sorted(c.items()):
        ts = [t for t, bb, w in runs if bb == b and len(w) == n]
        print(hex(b), n, 'words', k, 'times', '%.2f-%.2f' % (min(ts), max(ts)))
