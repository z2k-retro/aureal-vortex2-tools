#!/usr/bin/env python3
"""hrtf_extract.py - read the driver's HRTF directions out of a traced a3dtest run.

Aureal's driver turns each direction into 56 HRTF taps per ear and an interaural
delay (ITD), and writes them to the A3D source's target registers. Run
'A3DTEST hrtfmap' (or hrtfgrid, elev5) in a traced session, then:

  hrtf_extract.py TIMELINE A3DTEST.LOG OUT.csv [--test hrtfmap] [--slot 14] [--grid]

TIMELINE is timeline.txt from vortex_trace.py; the log is the one a3dtest wrote.
Each change of position produces one burst of target writes; bursts are matched to
the log's STEP lines in order (the guest clock drifts, so times are not used).
--grid keeps only entries on the driver's 15-degree grid.

CSV columns: az_deg, el_deg, itd_word (hex), itd_left_samples, itd_right_samples,
left0..left55, right0..right55 (signed Q15). tools/make_mhr.py turns a grid CSV
into an OpenAL Soft HRTF data set.
"""
import argparse, re, sys

ap = argparse.ArgumentParser()
ap.add_argument('timeline'); ap.add_argument('log'); ap.add_argument('out')
ap.add_argument('--test', default='hrtfmap')
ap.add_argument('--slot', type=int, default=14, help='A3D slot the test source uses (the driver hands out 14 first)')
ap.add_argument('--grid', action='store_true')
ap.add_argument('--gap', type=float, default=0.1, help='seconds of silence that separate bursts')
a = ap.parse_args()

steps = []
for line in open(a.log, errors='replace'):
    m = re.search(r'STEP %s n=\d+ .*?az=([-\d.]+) el=([-\d.]+)' % re.escape(a.test), line)
    if m:
        steps.append((float(m[1]), float(m[2])))
if not steps:
    sys.exit('no "STEP %s" lines in %s' % (a.test, a.log))

base = 0x19000 + (a.slot >> 2) * 0x2000 + (a.slot & 3) * 0x2c8   # bank B source base
lo, hi = base + 0x100, base + 0x1e8                                  # HrtfTarget .. ITDTarget
state, snaps, last_t = {}, [], None
for line in open(a.timeline, errors='replace'):
    f = line.split()
    if len(f) < 5 or f[1] != 'W':
        continue
    try:
        t, addr, val = float(f[0]), int(f[2], 16), int(f[4], 16)
    except ValueError:
        continue
    if not lo <= addr < hi:
        continue
    if last_t is not None and t - last_t > a.gap and state:
        snaps.append(dict(state))
    state[addr - base] = val
    last_t = t
if state:
    snaps.append(dict(state))

# The first burst can be the source starting up; keep the last len(steps) bursts.
if len(snaps) < len(steps):
    sys.exit('found %d bursts for %d steps; try another --slot or --gap' % (len(snaps), len(steps)))
if len(snaps) > len(steps):
    print('note: %d bursts for %d steps; using the last %d' % (len(snaps), len(steps), len(steps)), file=sys.stderr)
    snaps = snaps[-len(steps):]

def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v

n = 0
with open(a.out, 'w') as o:
    o.write('az_deg,el_deg,itd_word,itd_left_samples,itd_right_samples,'
            + ','.join('left%d' % i for i in range(56)) + ',' + ','.join('right%d' % i for i in range(56)) + '\n')
    seen = set()
    for (az, el), st in zip(steps, snaps):
        if a.grid and (az % 15 or el % 15):
            continue
        key = (az % 360, el)
        if key in seen:
            continue
        seen.add(key)
        w = [st.get(0x100 + k * 4, 0) for k in range(56)]
        itd = st.get(0x1e4, 0)
        o.write('%g,%g,0x%08x,%.4f,%.4f,' % (az % 360, el, itd, (itd & 0xffff) / 512, (itd >> 16) / 512)
                + ','.join(str(s16(x)) for x in w) + ',' + ','.join(str(s16(x >> 16)) for x in w) + '\n')
        n += 1
print('wrote %d directions to %s' % (n, a.out), file=sys.stderr)
