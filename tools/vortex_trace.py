#!/usr/bin/env python3
"""Parse QEMU vfio_region_read/write traces of an Aureal Vortex 2 (AU8830).

The Win98 driver reaches the chip's 256 KB register space two ways:
  * region1 (I/O BAR, 8 bytes): +0x0 = index (register address), +0x4 = data
  * region0 (MMIO BAR, 256 KB): direct access at the register offset
Both are normalised here into one stream of (time, op, reg, value) accesses.

Usage:
  vortex_trace.py summary LOG          per-register counts, first/last time
  vortex_trace.py timeline LOG [OUT]   register timeline, repeats collapsed
  vortex_trace.py csv LOG OUT          every normalised access as CSV
"""
import re
import sys
import csv
from collections import defaultdict, OrderedDict
from datetime import datetime

LINE = re.compile(
    r'^(?P<ts>\S+)\s+vfio_region_(?P<op>read|write)\s+'
    r'\(\S+:region(?P<bar>\d)\+0x(?P<off>[0-9a-f]+),\s*'
    r'(?:0x(?P<wval>[0-9a-f]+),\s*)?(?P<size>\d)\)'
    r'(?:\s*=\s*0x(?P<rval>[0-9a-f]+))?')


def parse_ts(s):
    # QEMU drops the fractional part when it is exactly .000000
    s = s.rstrip('Z')
    fmt = '%Y-%m-%dT%H:%M:%S.%f' if '.' in s else '%Y-%m-%dT%H:%M:%S'
    return datetime.strptime(s, fmt)


def accesses(path):
    """Yield dicts: t (seconds from start), op, reg, val, path ('io'/'mmio')."""
    t0 = None
    index = None
    with open(path, errors='replace') as f:
        for raw in f:
            m = LINE.match(raw)
            if not m:
                continue
            ts = parse_ts(m['ts'])
            if t0 is None:
                t0 = ts
            t = (ts - t0).total_seconds()
            bar, off, op = int(m['bar']), int(m['off'], 16), m['op']
            val = m['wval'] if op == 'write' else m['rval']
            val = int(val, 16) if val is not None else None
            if bar == 1:
                if off == 0 and op == 'write':
                    index = val
                    continue
                if off == 4:
                    yield dict(t=t, op=op, reg=index, val=val, path='io')
                    continue
                yield dict(t=t, op=op, reg=None, val=val, path='io+0x%x' % off)
            elif bar == 0:
                yield dict(t=t, op=op, reg=off, val=val, path='mmio')
            else:
                yield dict(t=t, op=op, reg=None, val=val, path='bar%d+0x%x' % (bar, off))


def fmt_reg(r):
    return '   ?   ' if r is None else '0x%05x' % r


def summary(path):
    stats = defaultdict(lambda: dict(r=0, w=0, first=None, last=None, vals=set(), path=set()))
    total = 0
    for a in accesses(path):
        total += 1
        s = stats[a['reg']]
        s['r' if a['op'] == 'read' else 'w'] += 1
        s['first'] = a['t'] if s['first'] is None else s['first']
        s['last'] = a['t']
        s['path'].add(a['path'])
        if len(s['vals']) < 9 and a['val'] is not None:
            s['vals'].add(a['val'])
    print('%d register accesses, %d distinct registers\n' % (total, len(stats)))
    print('%-8s %8s %8s %9s %9s  %-9s %s' % ('reg', 'reads', 'writes', 'first_s', 'last_s', 'via', 'sample values'))
    rows = sorted(stats.items(), key=lambda kv: -(kv[1]['r'] + kv[1]['w']))
    for reg, s in rows:
        vals = sorted(s['vals'])
        vs = ' '.join('%x' % v for v in vals[:8]) + (' …' if len(s['vals']) > 8 else '')
        print('%-8s %8d %8d %9.3f %9.3f  %-9s %s' % (
            fmt_reg(reg), s['r'], s['w'], s['first'], s['last'], '/'.join(sorted(s['path'])), vs))


def timeline(path, out):
    """Collapse runs of identical (op, reg, val) into one line with a count."""
    w = open(out, 'w') if out else sys.stdout
    prev = None
    count = 0
    start = 0.0

    def flush():
        if prev is None:
            return
        op, reg, val, p = prev
        v = '' if val is None else '0x%08x' % val
        rep = '' if count == 1 else '  x%d' % count
        arrow = 'W' if op == 'write' else 'R'
        w.write('%10.6f  %s %s %-4s %s%s\n' % (start, arrow, fmt_reg(reg), p, v, rep))

    for a in accesses(path):
        key = (a['op'], a['reg'], a['val'], a['path'])
        if key == prev:
            count += 1
            continue
        flush()
        prev, count, start = key, 1, a['t']
    flush()
    if out:
        w.close()


def to_csv(path, out):
    with open(out, 'w', newline='') as f:
        c = csv.writer(f)
        c.writerow(['t_s', 'op', 'reg', 'value', 'via'])
        for a in accesses(path):
            c.writerow(['%.6f' % a['t'], a['op'],
                        '' if a['reg'] is None else '0x%05x' % a['reg'],
                        '' if a['val'] is None else '0x%08x' % a['val'], a['path']])


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, log = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if cmd == 'summary':
        summary(log)
    elif cmd == 'timeline':
        timeline(log, out)
    elif cmd == 'csv':
        if not out:
            sys.exit('csv needs an output path')
        to_csv(log, out)
    else:
        sys.exit(__doc__)
