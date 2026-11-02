#!/usr/bin/env python3
"""Track per-source A3D parameters from a vortex_trace timeline.
Source = coefficient block of 56 words at BASE; 8-word parameter gap at BASE-0x20..BASE-0x4."""
import sys, csv
def s16(x): return x - 0x10000 if x & 0x8000 else x
def rows(path):
    for l in open(path, errors='replace'):
        f = l.split()
        if len(f) < 5 or f[1] != 'W' or f[2] == '?': continue
        try: yield float(f[0]), int(f[2], 16), int(f[4], 16)
        except ValueError: pass
path, out = sys.argv[1], sys.argv[2]
bases = [int(b, 16) for b in sys.argv[3:]]
state = {}
recs = []
for t, r, v in rows(path):
    state[r] = v
    for b in bases:
        # snapshot when last coefficient word of block written
        if r == b + 55*4:
            hdr = [state.get(b - 0x20 + 4*i) for i in range(8)]
            co = [state.get(b + 4*i, 0) for i in range(56)]
            hi = [s16(c >> 16) for c in co]; lo = [s16(c & 0xffff) for c in co]
            eh = sum(x*x for x in hi); el = sum(x*x for x in lo)
            pk_h = max(range(56), key=lambda i: abs(hi[i])); pk_l = max(range(56), key=lambda i: abs(lo[i]))
            recs.append([round(t, 4), hex(b)] + [('%08x' % h) if h is not None else '' for h in hdr] + [eh, el, pk_h, pk_l] + hi + lo)
with open(out, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['t', 'base'] + ['p%d' % i for i in range(8)] + ['energy_hi', 'energy_lo', 'peak_hi', 'peak_lo'] + ['hi%d' % i for i in range(56)] + ['lo%d' % i for i in range(56)])
    w.writerows(recs)
print(len(recs), 'updates')
