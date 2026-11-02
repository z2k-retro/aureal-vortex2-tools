#!/usr/bin/env python3
"""vortex-poke.py - read and write Aureal Vortex 2 registers from the host while the
traced VM is running, through the card's BAR0 (sysfs resource0).

Host writes do not appear in the QEMU trace, so every write is logged here with a UTC
timestamp (the same clock as qemu-trace.log) for alignment.

  sudo ./vortex-poke.py peek ADDR [COUNT]       read COUNT 32-bit words (default 1)
  sudo ./vortex-poke.py poke ADDR VALUE          write one 32-bit word
  sudo ./vortex-poke.py watch                    print slot 14's gain target and time constants twice a second
  sudo ./vortex-poke.py tcsweep [--log FILE]     run alongside 'A3DTEST tcsweep' in the guest
  sudo ./vortex-poke.py xtsweep [--log FILE]     run alongside 'A3DTEST click90': rewrites the crosstalk
                                                 canceller section by section, 4 s each (about 1 minute),
                                                 then restores every word it read
  sudo ./vortex-poke.py xtident [--log FILE]     run alongside 'A3DTEST click90': synthetic canceller
                                                 coefficients, one word at a time, 3 s each (~1.5 min)
  sudo ./vortex-poke.py xtident2 [--log FILE]    the same, re-arming the canceller after every change
                                                 (bit 0 of 0x24660 off then on, as the driver does)
  sudo ./vortex-poke.py glidewatch [--log FILE] [--out FILE]
                                                 run alongside 'A3DTEST tcsweep': like tcsweep, but after
                                                 every target change reads the chip's *current* gain, filter,
                                                 HRTF and ITD registers every ~100 us for 1 s (binary samples
                                                 in glide.bin, events in the log), so the glide law is read
                                                 straight off the hardware instead of out of the audio
  sudo ./vortex-poke.py eqsweep [--log FILE]     start once 'A3DTEST noise2d' is playing: switches the
                                                 chip's EQ in band by band, 4 s each (about 1 minute)

Only the A3D per-source time-constant words are touched by tcsweep, and they are put
back to the driver's values at the end (or on Ctrl-C).

Environment: HOSTDEV (default 0000:0c:01.0).
"""
import ctypes, datetime, json, mmap, os, sys, time

DEV = os.environ.get('HOSTDEV', '0000:0c:01.0')
RES = f'/sys/bus/pci/devices/{DEV}/resource0'
SIZE = 0x40000

SLOT = 14                                   # first slot the driver hands out (slice 3, source 2)
SL, SR = divmod(SLOT, 4)
BANK_A = 0x18000 + SL * 0x2000 + SR * 0x3a4
BANK_B = 0x19000 + SL * 0x2000 + SR * 0x2c8
TC = {  # ALSA names and offsets (bank A); values are the Windows driver's (= ALSA defaults)
    'hrtf': (BANK_A + 0x37c, 0x147),
    'gain': (BANK_A + 0x380, 0x300),
    'coef': (BANK_A + 0x384, 0x300),
    'itd':  (BANK_A + 0x388, 0x0c8),
}
GAIN_TARGET = BANK_B + 0xe4
GAIN_LOW = 0x0ccc                            # SetGain(0.1): first event of every guest cycle
# the driver writes 0x0ccc0ccc but the host reads back only the low 16 bits, so compare those

# (word, value): each held for two guest cycles; others at the driver's values
SCHEDULE = [('base', None)] + \
    [('gain', v) for v in (0x100, 0x180, 0x600, 0xc00)] + \
    [('coef', v) for v in (0x100, 0x180, 0x600, 0xc00)] + \
    [('hrtf', v) for v in (0x52, 0x51e, 0xc00)] + \
    [('itd', v) for v in (0x32, 0x320, 0xc00)] + \
    [('base', None)]
CYCLES_PER_CONFIG = 2


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='microseconds').replace('+00:00', 'Z')


class Bar:
    def __init__(self):
        self.fd = os.open(RES, os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.mm))

    def rd(self, addr):                     # single aligned 32-bit load
        return ctypes.c_uint32.from_address(self.base + addr).value

    def wr(self, addr, val):                # single aligned 32-bit store
        ctypes.c_uint32.from_address(self.base + addr).value = val & 0xffffffff


def check_addr(a):
    if a % 4 or not 0 <= a < SIZE:
        sys.exit(f'bad address {a:#x}')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    bar = Bar()
    if cmd == 'peek':
        a = int(sys.argv[2], 0); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 1
        check_addr(a)
        for i in range(n):
            print(f'{a + 4 * i:#07x}: {bar.rd(a + 4 * i):#010x}')
        return
    if cmd == 'poke':
        a = int(sys.argv[2], 0); v = int(sys.argv[3], 0)
        check_addr(a)
        old = bar.rd(a); bar.wr(a, v); new = bar.rd(a)
        print(json.dumps({'utc': utc(), 'addr': hex(a), 'old': hex(old), 'wrote': hex(v), 'readback': hex(new)}))
        return
    if cmd == 'watch':                      # quick check that host reads see the guest's writes
        try:
            while True:
                tc = ' '.join(f'{k}={bar.rd(a):#x}' for k, (a, _) in TC.items())
                print(f'{utc()} slot {SLOT} gain target {bar.rd(GAIN_TARGET):#010x}  {tc}', flush=True)
                time.sleep(0.5)
        except KeyboardInterrupt:
            return
    logpath = sys.argv[sys.argv.index('--log') + 1] if '--log' in sys.argv else 'poke-log.jsonl'
    if cmd == 'xtsweep':
        log = open(logpath, 'a')
        def note(**kw):
            kw['utc'] = utc(); line = json.dumps(kw); log.write(line + '\n'); log.flush(); print(line)
        xtsweep(bar, note)
        return
    if cmd in ('xtident', 'xtident2'):
        log = open(logpath, 'a')
        def note(**kw):
            kw['utc'] = utc(); line = json.dumps(kw); log.write(line + '\n'); log.flush(); print(line)
        xtident(bar, note, latch=(cmd == 'xtident2'))
        return
    if cmd == 'eqsweep':
        log = open(logpath, 'a')
        def note(**kw):
            kw['utc'] = utc(); line = json.dumps(kw); log.write(line + '\n'); log.flush(); print(line)
        eqsweep(bar, note)
        return
    if cmd == 'glidewatch':
        outpath = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else 'glide.bin'
        log = open(logpath, 'a')
        def note(**kw):
            kw['utc'] = utc(); line = json.dumps(kw); log.write(line + '\n'); log.flush(); print(line)
        glidewatch(bar, note, outpath)
        return
    if cmd != 'tcsweep':
        sys.exit(__doc__)

    logpath = sys.argv[sys.argv.index('--log') + 1] if '--log' in sys.argv else 'poke-log.jsonl'
    log = open(logpath, 'a')
    def note(**kw):
        kw['utc'] = utc(); line = json.dumps(kw); log.write(line + '\n'); log.flush(); print(line)

    def set_defaults():
        for name, (addr, val) in TC.items():
            bar.wr(addr, val)

    print(f'waiting for the guest source in slot {SLOT} (start "A3DTEST run3" in Windows)...')
    print('Start this script fresh for each VM session: after QEMU restarts, re-run it.')
    while True:                             # source active and time constants as the driver set them
        vals = {k: bar.rd(a) for k, (a, _) in TC.items()}
        if all(vals[k] == TC[k][1] for k in TC) and bar.rd(GAIN_TARGET) != 0:
            break
        time.sleep(0.05)
    note(event='source_found', slot=SLOT, tc={k: hex(v) for k, v in vals.items()})

    def wait_cycle_start():
        prev = bar.rd(GAIN_TARGET) & 0xffff; beat = time.time()
        while True:
            cur = bar.rd(GAIN_TARGET) & 0xffff
            if cur == GAIN_LOW and prev != GAIN_LOW:
                return
            if time.time() - beat > 5:          # heartbeat, so a stalled wait is visible
                beat = time.time()
                state = 'card not readable (VM stopped or restarted?)' if cur == 0xffff and bar.rd(GAIN_TARGET) == 0xffffffff else 'waiting for the next cycle'
                print(f'  {utc()} gain target {cur:#06x}: {state}', flush=True)
            prev = cur
            time.sleep(0.002)

    try:
        for i, (word, val) in enumerate(SCHEDULE):
            wait_cycle_start()
            set_defaults()
            if word != 'base':
                bar.wr(TC[word][0], val)
            note(event='config', index=i, word=word, value=hex(val) if val is not None else None,
                 readback={k: hex(bar.rd(a)) for k, (a, _) in TC.items()})
            for _ in range(CYCLES_PER_CONFIG - 1):
                wait_cycle_start()
                note(event='cycle', index=i)
    except KeyboardInterrupt:
        note(event='interrupted')
    finally:
        set_defaults()
        note(event='restored', tc={k: hex(bar.rd(a)) for k, (a, _) in TC.items()})


# ---------------------------------------------------------------- glide readback
# Targets watched for a change, and the current registers read after it (bank B offsets).
GLIDE_TARGETS = {'gain': 0xe4, 'coef': 0xf4, 'itd': 0x1e4, 'hrtf': 0x100}
GLIDE_CURRENT = [0xe0, 0xe8, 0xf0, 0x1e0, 0x000, 0x004, 0x008, 0x00c]
GLIDE_SECONDS = 1.0
GLIDE_PERIOD = 100e-6

def glidewatch(bar, note, outpath):
    import struct
    out = open(outpath, 'ab')
    def set_defaults():
        for name, (addr, val) in TC.items():
            bar.wr(addr, val)
    print(f'waiting for the guest source in slot {SLOT} (start "A3DTEST tcsweep" in Windows)...')
    while True:
        vals = {k: bar.rd(a) for k, (a, _) in TC.items()}
        if all(vals[k] == TC[k][1] for k in TC) and bar.rd(GAIN_TARGET) != 0:
            break
        time.sleep(0.05)
    note(event='source_found', slot=SLOT, tc={k: hex(v) for k, v in vals.items()})
    tgt = {k: BANK_B + o for k, o in GLIDE_TARGETS.items()}
    cur = [BANK_B + o for o in GLIDE_CURRENT]
    def capture(what, old, new, config):
        t0 = time.perf_counter(); rows = []
        nxt = t0
        while True:
            now = time.perf_counter()
            if now - t0 > GLIDE_SECONDS:
                break
            if now >= nxt:
                rows.append((now - t0, [bar.rd(a) for a in cur]))
                nxt += GLIDE_PERIOD
        hdr = json.dumps({'what': what, 'old': hex(old), 'new': hex(new), 'config': config,
                          'n': len(rows), 'regs': [hex(o) for o in GLIDE_CURRENT]}).encode()
        out.write(struct.pack('<I', len(hdr)) + hdr)
        for t, v in rows:
            out.write(struct.pack('<d' + 'I' * len(v), t, *v))
        out.flush()
        note(event='glide', what=what, old=hex(old), new=hex(new), config=config, samples=len(rows))
    try:
        for i, (word, val) in enumerate(SCHEDULE):
            set_defaults()
            if word != 'base':
                bar.wr(TC[word][0], val)
            config = f'{word}={hex(val) if val is not None else "default"}'
            note(event='config', index=i, word=word, value=hex(val) if val is not None else None,
                 readback={k: hex(bar.rd(a)) for k, (a, _) in TC.items()})
            last = {k: bar.rd(a) for k, a in tgt.items()}
            got = 0; beat = time.time()
            while got < 12:                 # two guest cycles of six changes each
                for k, a in tgt.items():
                    v = bar.rd(a)
                    if v == 0xffffffff:
                        raise RuntimeError('card not readable (VM stopped?)')
                    if v != last[k]:
                        capture(k, last[k], v, config); got += 1
                        last = {kk: bar.rd(aa) for kk, aa in tgt.items()}
                        break
                if time.time() - beat > 20:
                    print(f'  {utc()} no target change for 20 s; is A3DTEST tcsweep running?', flush=True)
                    beat = time.time()
                time.sleep(0.0005)
    except KeyboardInterrupt:
        note(event='interrupted')
    finally:
        set_defaults()
        note(event='restored', tc={k: hex(bar.rd(a)) for k, (a, _) in TC.items()})


# ---------------------------------------------------------------- crosstalk canceller sweep
# Register layout from ALSA au88x0_xtalk.c: per filter five sections of five words,
# sections 0x24 apart; gains (K) and shifts at 0x24538-0x24554; delay at 0x24660.
XT_FILTERS = {'eqL': 0x24200, 'eqR': 0x242b4, 'xtL': 0x24368, 'xtR': 0x2441c}
XT_GAINS = {'eqL': (0x24538, 0x2453c), 'eqR': (0x24540, 0x24544), 'xtL': (0x24548, 0x2454c), 'xtR': (0x24550, 0x24554)}
PIPE = [(0, 0, 0x0fa0, 0, 0)] * 4 + [(0, 0, 0x1180, 0, 0)]      # the driver's headphone (pass-through) sections
XT_SPAN = (0x24200, 0x24560)
ITD_TARGET = BANK_B + 0x1e4


def xt_rows(filt, rows):
    base = XT_FILTERS[filt]
    return {base + i * 0x24 + j * 4: rows[i][j] for i in range(5) for j in range(5)}


def xt_schedule(orig):
    """Each entry: (name, {addr: value}) applied on top of the captured original block."""
    rd = lambda a: orig[a]
    rows = {f: [tuple(rd(XT_FILTERS[f] + i * 0x24 + j * 4) for j in range(5)) for i in range(5)] for f in XT_FILTERS}
    z = lambda f: {XT_GAINS[f][0]: 0}                               # silence a filter via its gain
    sched = [('base', {})]
    sched.append(('eq only (xt gains 0)', {**z('xtL'), **z('xtR')}))
    sched.append(('xt only (eq gains 0)', {**z('eqL'), **z('eqR')}))
    sched.append(('eqL all pipe, xt off', {**xt_rows('eqL', PIPE), **z('xtL'), **z('xtR')}))
    for i in range(5):                                              # one EQ section alone (left ear only)
        r = list(PIPE); r[i] = rows['eqL'][i]
        sched.append((f'eqL section {i} alone, xt off', {**xt_rows('eqL', r), **z('xtL'), **z('xtR')}))
    for i in range(4):                                              # one XT section alone, EQ off
        r = list(PIPE); r[i] = rows['xtL'][i]; r[4] = rows['xtL'][4]     # XT's fifth section is zero in the driver's sets
        sched.append((f'xtL section {i} alone, eq off', {**xt_rows('xtL', r), **z('eqL'), **z('eqR')}))
    k, sh = XT_GAINS['eqL']
    sched.append(('eqL gain halved', {k: (rd(k) >> 1) & 0xffff}))
    sched.append(('eqL shift +1', {sh: rd(sh) + 1}))
    sched.append(('base again', {}))
    return sched


def xtsweep(bar, note, hold=4.0):
    print('waiting for the guest click source at 90 degrees (run "A3DTEST click90" in Windows)...')
    while not (bar.rd(ITD_TARGET) & 0xffff == 0x4600 and bar.rd(GAIN_TARGET) & 0xffff):
        time.sleep(0.05)
    time.sleep(1.0)
    orig = {a: bar.rd(a) & 0xffff for a in range(*XT_SPAN, 4)}
    orig[0x24660] = bar.rd(0x24660)
    note(event='xt_captured', words=len(orig), sample={hex(a): hex(orig[a]) for a in (0x24200, 0x24538, 0x2453c, 0x24548, 0x24660)})
    sched = xt_schedule(orig)

    def restore():
        for a, val in orig.items():
            bar.wr(a, val)
    try:
        for i, (name, writes) in enumerate(sched):
            restore()
            for a, val in writes.items():
                bar.wr(a, val)
            note(event='xt_config', index=i, name=name, writes={hex(a): hex(v) for a, v in writes.items()})
            time.sleep(hold)
    except KeyboardInterrupt:
        note(event='interrupted')
    finally:
        restore()
        note(event='xt_restored', check={hex(a): hex(bar.rd(a) & 0xffff) for a in (0x24200, 0x24538, 0x24548)})


# ---------------------------------------------------------------- EQ sweep
# ALSA au88x0_eq.c: band gain targets 0x2b02c + band*0x30 (left) and 0x2b20c + band*0x30
# (right); bypass gain 0x2b3d4 (left) / 0x2b3ec (right). The Windows driver leaves every band
# at 0 and the bypass at 0x7fff, i.e. the EQ is bypassed.
EQ_BAND = lambda side, i: (0x2b02c if side == 'L' else 0x2b20c) + i * 0x30
EQ_BYPASS = (0x2b3d4, 0x2b3ec)
EQ_ON = 0x3e96                                                     # ALSA's "normal" band gain


def eqsweep(bar, note, hold=4.0):
    addrs = [EQ_BAND(s, i) for s in 'LR' for i in range(10)] + list(EQ_BYPASS)
    orig = {a: bar.rd(a) & 0xffff for a in addrs}
    note(event='eq_captured', bands_left=[hex(orig[EQ_BAND('L', i)]) for i in range(10)],
         bypass=[hex(orig[a]) for a in EQ_BYPASS])
    both = lambda i, val: {EQ_BAND('L', i): val, EQ_BAND('R', i): val}
    sched = [('base', {}),
             ('bypass 0, bands 0', {EQ_BYPASS[0]: 0, EQ_BYPASS[1]: 0}),
             ('bypass 0, all bands normal', {**{k: v for i in range(10) for k, v in both(i, EQ_ON).items()},
                                            EQ_BYPASS[0]: 0, EQ_BYPASS[1]: 0})]
    for i in range(10):
        sched.append((f'bypass 0, band {i} only', {**both(i, EQ_ON), EQ_BYPASS[0]: 0, EQ_BYPASS[1]: 0}))
    sched.append(('bypass 0x3fff (half)', {EQ_BYPASS[0]: 0x3fff, EQ_BYPASS[1]: 0x3fff}))
    sched.append(('base again', {}))

    def restore():
        for a, val in orig.items():
            bar.wr(a, val)
    try:
        for i, (name, writes) in enumerate(sched):
            restore()
            for a, val in writes.items():
                bar.wr(a, val)
            note(event='eq_config', index=i, name=name, writes={hex(a): hex(v) for a, v in writes.items()})
            time.sleep(hold)
    except KeyboardInterrupt:
        note(event='interrupted')
    finally:
        restore()
        note(event='eq_restored', bypass=[hex(bar.rd(a) & 0xffff) for a in EQ_BYPASS])


# ---------------------------------------------------------------- canceller system identification
# Synthetic coefficients chosen so each step isolates one word: pure gains, pure delays and
# gentle one-pole sections whose poles stay inside the unit circle whatever the fixed-point
# scale turns out to be (|a| <= 0.5 even if the words were Q12).
UNIT = (0, 0, 0x1000, 0, 0)                  # b0 only

def xtident_schedule():
    neutral = lambda f: {**xt_rows(f, [UNIT] * 5)}
    off = {XT_GAINS['xtL'][0]: 0, XT_GAINS['xtR'][0]: 0}
    passthru = {XT_GAINS['eqL'][0]: 0, XT_GAINS['eqR'][0]: 0, **off}           # what headphone mode does
    sched = [('passthrough (all K 0)', passthru)]
    # EQ gain word K and shift, left side only, sections all UNIT
    for k in (0x1000, 0x2000, 0x4000):
        for sh in (0, 1):
            sched.append((f'eqL K={k:#x} shift={sh}, unit sections', {**passthru, **neutral('eqL'),
                          XT_GAINS['eqL'][0]: k, XT_GAINS['eqL'][1]: sh}))
    base = {**passthru, **neutral('eqL'), XT_GAINS['eqL'][0]: 0x4000, XT_GAINS['eqL'][1]: 0}
    variants = [('b0=0x2000', (0, 0, 0x2000, 0, 0)), ('b0=-0x1000', (0, 0, 0xf000, 0, 0)),
                ('b1 only', (0, 0, 0, 0x1000, 0)), ('b2 only', (0, 0, 0, 0, 0x1000)),
                ('a1=+0x0800', (0x0800, 0, 0x1000, 0, 0)), ('a1=-0x0800', (0xf800, 0, 0x1000, 0, 0)),
                ('a2=+0x0800', (0, 0x0800, 0x1000, 0, 0)), ('b0+b1', (0, 0, 0x1000, 0x1000, 0))]
    for name, row in variants:
        rows = [UNIT] * 5; rows[0] = row
        sched.append((f'eqL section0 {name}', {**base, **xt_rows('eqL', rows)}))
    rows = [UNIT] * 5; rows[4] = (0, 0, 0x2000, 0, 0)
    sched.append(('eqL section4 b0=0x2000', {**base, **xt_rows('eqL', rows)}))
    rows = [UNIT] * 5; rows[4] = (0, 0, 0, 0, 0)
    sched.append(('eqL section4 all zero', {**base, **xt_rows('eqL', rows)}))
    # crosstalk path: EQs pass through, xtL (left -> right output) with unit sections
    xbase = {**passthru, **neutral('xtL'), XT_GAINS['xtL'][0]: 0x4000, XT_GAINS['xtL'][1]: 0}
    sched.append(('xtL K=0x4000 shift 0, unit sections', xbase))
    sched.append(('xtL K=0x4000 shift 1', {**xbase, XT_GAINS['xtL'][1]: 1}))
    for name, row in variants[:3] + variants[4:5]:
        rows = [UNIT] * 5; rows[0] = row
        sched.append((f'xtL section0 {name}', {**xbase, **xt_rows('xtL', rows)}))
    sched.append(('xtL with EQ left active (eqL K=0x4000, unit)', {**xbase, **neutral('eqL'), XT_GAINS['eqL'][0]: 0x4000}))
    sched.append(('passthrough again', passthru))
    return sched


XT_CTRL = 0x24660        # driver writes ...ad00, ...ad88, then ...ad89 after loading coefficients


def xt_latch(bar, ctrl):
    """Re-arm the canceller the way the driver does after loading coefficients:
    bit 0 cleared, then set again."""
    bar.wr(XT_CTRL, ctrl & ~1)
    time.sleep(0.002)
    bar.wr(XT_CTRL, ctrl | 1)


def xtident(bar, note, hold=3.0, latch=False):
    print('waiting for the guest click source at 90 degrees (run "A3DTEST click90" in Windows)...')
    while not (bar.rd(ITD_TARGET) & 0xffff == 0x4600 and bar.rd(GAIN_TARGET) & 0xffff):
        time.sleep(0.05)
    time.sleep(1.0)
    orig = {a: bar.rd(a) & 0xffff for a in range(*XT_SPAN, 4)}
    ctrl = bar.rd(XT_CTRL)
    note(event='xi_captured', words=len(orig), ctrl=hex(ctrl), latch=latch)
    def restore():
        for a, val in orig.items():
            bar.wr(a, val)
        if latch:
            xt_latch(bar, ctrl)
    try:
        for i, (name, writes) in enumerate(xtident_schedule()):
            restore()
            for a, val in writes.items():
                bar.wr(a, val)
            if latch:
                xt_latch(bar, ctrl)
            back = {hex(a): hex(bar.rd(a) & 0xffff) for a in list(writes)[:6]}
            note(event='xi_config', index=i, name=name, latch=latch,
                 writes={hex(a): hex(v) for a, v in writes.items()}, readback=back, ctrl=hex(bar.rd(XT_CTRL)))
            time.sleep(hold)
    except KeyboardInterrupt:
        note(event='interrupted')
    finally:
        restore()
        note(event='xi_restored')


if __name__ == '__main__':
    main()
