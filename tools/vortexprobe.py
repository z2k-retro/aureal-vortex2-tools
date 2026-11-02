#!/usr/bin/env python3
"""vortexprobe.py - documentation probes for the Aureal Vortex 2 (AU8830).

Run with sudo (it reads PCI config space, the QEMU control socket and, for
'cold', the card's registers directly). Output files are handed back to you.

  sudo python3 vortexprobe.py pci                 PCI identity, config space, BARs, IRQ, board info
  sudo python3 vortexprobe.py snap NAME           read all 256 KB of registers from the running VM
  sudo python3 vortexprobe.py cold                read registers from Linux before Windows has run
       python3 vortexprobe.py diff A.bin B.bin    compare two register snapshots
       python3 vortexprobe.py codec TIMELINE      AC'97 codec ID and register settings from a trace
       python3 vortexprobe.py decode SNAP.bin     routing graph, SRC rates, DMA setup, mixer volumes
  sudo python3 vortexprobe.py clock               measure the card's sample-counter clock (VM running)

Options: --dev 0000:0c:01.0   --out DIR   --qmp SOCKET (default ~/traces/qmp.sock)
'snap' needs the VM started by vortex-session.sh (which opens the QMP socket).
"""
import argparse, datetime as dt, json, mmap, os, pwd, re, socket, struct, subprocess, sys, ctypes

BAR0_SIZE = 0x40000        # 256 KB register window
WORDS = BAR0_SIZE // 4

# Names from the Linux ALSA au8830.h (only ones confirmed there) plus regions found in our traces.
KNOWN = [
    (0x10000, 0x10000, 'FIFO_WTDATA (ALSA)'),
    (0x17000, 0x17000, 'FIFO_GIRT (ALSA)'),
    (0x18000, 0x1ffff, 'A3D filter banks (traced: 4 banks x 4 groups, 56-word HRTF blocks)'),
    (0x28810, 0x2881c, 'GAME_AXIS (ALSA)'),
    (0x29080, 0x29080, 'CODEC_CHN (ALSA)'),
    (0x29184, 0x29184, 'CODEC_CTRL (ALSA)'),
    (0x29188, 0x29188, 'CODEC_IO (ALSA): bit23 write, bits22-16 AC97 reg, bits15-0 data'),
    (0x2918c, 0x2918c, 'CODEC_SPORTCTRL (ALSA)'),
    (0x29190, 0x29190, 'CODEC_EN (ALSA)'),
    (0x29198, 0x2919c, 'heavily polled in traces (counter/status?)'),
    (0x2a000, 0x2a00c, 'interrupt/control block (traced)'),
]

AC97_VENDORS = {0x83847608: 'SigmaTel STAC9708/9711', 0x83847609: 'SigmaTel STAC9721/23',
                0x41445340: 'Analog Devices AD1881', 0x414b4d00: 'AKM AK4540'}


def label(addr):
    for lo, hi, name in KNOWN:
        if lo <= addr <= hi:
            return name
    return ''


def real_user():
    name = os.environ.get('SUDO_USER') or pwd.getpwuid(os.getuid()).pw_name
    pw = pwd.getpwnam(name)
    return pw.pw_uid, pw.pw_gid, pw.pw_dir


def outdir(args, sub):
    uid, gid, home = real_user()
    d = args.out or os.path.join(home, 'traces', sub)
    os.makedirs(d, exist_ok=True)
    return d


def give_back(*paths):
    uid, gid, _ = real_user()
    for p in paths:
        try:
            os.chown(p, uid, gid)
        except OSError:
            pass


def now():
    return dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%fZ')


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    except Exception as e:
        return f'<failed: {e}>\n'


def need_root():
    if os.geteuid() != 0:
        sys.exit('run this with sudo')


# ---------------------------------------------------------------- pci
def cmd_pci(args):
    need_root()
    d = outdir(args, 'probe-' + dt.datetime.now().strftime('%Y%m%d-%H%M%S'))
    dev = args.dev
    sysd = f'/sys/bus/pci/devices/{dev}'
    files = {
        'lspci-vvv-xxxx.txt': run(['lspci', '-vvv', '-xxxx', '-nn', '-s', dev]),
        'lspci-tree.txt': run(['lspci', '-tvnn']),
        'board.txt': run(['dmidecode', '-t', 'baseboard', '-t', 'bios']),
        'interrupts.txt': run(['grep', '-iE', 'vfio|au88|CPU', '/proc/interrupts']),
        'kernel.txt': run(['uname', '-a']),
    }
    for name, text in files.items():
        p = os.path.join(d, name)
        open(p, 'w').write(text)
        give_back(p)
    cfg = open(f'{sysd}/config', 'rb').read()
    p = os.path.join(d, 'config-space.bin'); open(p, 'wb').write(cfg); give_back(p)

    def r(path):
        try:
            return open(path).read().strip()
        except OSError:
            return None
    res = []
    for i, line in enumerate((r(f'{sysd}/resource') or '').splitlines()[:7]):
        s, e, fl = (int(x, 16) for x in line.split())
        if s or e:
            res.append({'bar': i, 'start': hex(s), 'size': hex(e - s + 1), 'flags': hex(fl),
                        'type': 'io' if fl & 0x100 else 'mem'})
    w = lambda o: int.from_bytes(cfg[o:o + 2], 'little')
    info = {
        'time': now(), 'device': dev,
        'vendor': hex(w(0)), 'device_id': hex(w(2)), 'revision': hex(cfg[8]),
        'class': hex(int.from_bytes(cfg[9:12], 'little')),
        'subsystem': f'{w(0x2c):04x}:{w(0x2e):04x}',
        'header_type': hex(cfg[0x0e]), 'cache_line': cfg[0x0c], 'latency_timer': cfg[0x0d],
        'interrupt_pin': cfg[0x3d], 'min_gnt': cfg[0x3e], 'max_lat': cfg[0x3f],
        'expansion_rom': hex(int.from_bytes(cfg[0x30:0x34], 'little')),
        'cfg_0x40': hex(int.from_bytes(cfg[0x40:0x44], 'little')),
        'driver': os.path.basename(os.path.realpath(f'{sysd}/driver')) if os.path.exists(f'{sysd}/driver') else None,
        'iommu_group': os.path.basename(os.path.realpath(f'{sysd}/iommu_group')) if os.path.exists(f'{sysd}/iommu_group') else None,
        'resources': res,
    }
    caps, ptr = [], cfg[0x34]
    while ptr and ptr < len(cfg) and len(caps) < 16:
        caps.append({'offset': hex(ptr), 'id': hex(cfg[ptr]),
                     'data': cfg[ptr:ptr + 8].hex()})
        ptr = cfg[ptr + 1]
    info['capabilities'] = caps
    p = os.path.join(d, 'pci-summary.json'); json.dump(info, open(p, 'w'), indent=2); give_back(p, d)
    print(json.dumps(info, indent=2))
    print(f'\nsaved to {d}')


# ---------------------------------------------------------------- QMP snapshot
class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rwb')
        self._read()
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            m = json.loads(self.f.readline())
            if 'event' not in m:
                return m

    def cmd(self, name, **a):
        self.f.write(json.dumps({'execute': name, 'arguments': a}).encode() + b'\n'); self.f.flush()
        m = self._read()
        if 'error' in m:
            raise RuntimeError(m['error'])
        return m['return']

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


def find_bar0(q):
    for bus in q.cmd('query-pci'):
        for dev in bus['devices']:
            if dev['id']['vendor'] == 0x12eb and dev['id']['device'] == 0x0002:
                for reg in dev['regions']:
                    if reg['bar'] == 0 and reg['type'] == 'memory':
                        if reg['address'] < 0:
                            raise SystemExit('BAR0 not assigned yet - let the VM boot first')
                        return reg['address'], reg['size']
    raise SystemExit('Vortex not found in the VM (is it running with the card passed through?)')


def save_snapshot(d, name, words, meta):
    b = os.path.join(d, name + '.bin')
    open(b, 'wb').write(struct.pack(f'<{len(words)}I', *words))
    t = os.path.join(d, name + '.txt')
    with open(t, 'w') as f:
        for i in range(0, len(words), 4):
            row = words[i:i + 4]
            if any(row):
                f.write(f'{i*4:05x}: ' + ' '.join(f'{x:08x}' for x in row) + '\n')
    j = os.path.join(d, name + '.json'); json.dump(meta, open(j, 'w'), indent=2)
    give_back(b, t, j, d)
    nz = sum(1 for x in words if x)
    print(f'{name}: {len(words)} words, {nz} non-zero -> {b}')


def cmd_snap(args):
    need_root()
    uid, gid, home = real_user()
    sock = args.qmp or os.path.join(home, 'traces', 'qmp.sock')
    if not os.path.exists(sock):
        sys.exit(f'no QMP socket at {sock} - start the VM with vortex-session.sh')
    cur = os.path.join(home, 'traces', 'current')
    d = args.out or (os.path.realpath(cur) if os.path.exists(cur) else outdir(args, 'snapshots'))
    os.makedirs(d, exist_ok=True)
    q = QMP(sock)
    base, size = find_bar0(q)
    t0 = now()
    words, chunk = [], 1024
    pat = re.compile(r'^[0-9a-f]+:\s+(.*)$')
    for off in range(0, BAR0_SIZE, chunk * 4):
        out = q.hmp(f'xp /{chunk}xw {base + off:#x}')
        got = []
        for line in out.splitlines():
            m = pat.match(line.strip())
            if m:
                got += [int(x, 16) for x in m.group(1).split()]
        if len(got) != chunk:
            sys.exit(f'unexpected xp output at offset {off:#x}: {out[:200]!r}')
        words += got
    meta = {'name': args.name, 'source': 'VM (QMP xp through vfio)', 'bar0_guest_addr': hex(base),
            'start': t0, 'end': now(),
            'note': 'every read here also appears in qemu-trace.log between start and end'}
    save_snapshot(d, 'snap-' + args.name, words, meta)


# ---------------------------------------------------------------- cold read
def cmd_cold(args):
    need_root()
    dev, sysd = args.dev, f'/sys/bus/pci/devices/{args.dev}'
    if subprocess.run(['pgrep', '-f', 'qemu-system'], capture_output=True).returncode == 0:
        sys.exit('a QEMU is running - shut it down first')
    print('Reads all 256 KB of registers directly. Best right after a cold boot, before Windows has run.')
    print('A few registers may change state when read; worst case the card or PC needs a reboot.')
    if input('Type YES to continue: ') != 'YES':
        sys.exit('cancelled')
    import time
    # Let the kernel bring the card to D0: with NoSoftRst-, a D3->D0 transition done by hand
    # wipes the BARs, and every read then returns 0xffffffff. power/control=on makes the
    # kernel wake the device and restore its config space properly.
    pctl = f'{sysd}/power/control'
    pctl_old = open(pctl).read().strip() if os.path.exists(pctl) else None
    if pctl_old is not None and pctl_old != 'on':
        open(pctl, 'w').write('on')
        time.sleep(0.2)
    cfgf = os.open(f'{sysd}/config', os.O_RDWR)
    cfg = bytearray(os.pread(cfgf, 256, 0))
    saved_hdr = bytes(cfg[0x10:0x40])
    cmd_old = int.from_bytes(cfg[4:6], 'little')
    pm = cfg[0x34]
    while pm and cfg[pm] != 0x01:
        pm = cfg[pm + 1]
    pmcsr_old = int.from_bytes(cfg[pm + 4:pm + 6], 'little') if pm else None
    try:
        if pm and pmcsr_old & 3:
            os.pwrite(cfgf, (pmcsr_old & ~3).to_bytes(2, 'little'), pm + 4)   # D0
            time.sleep(0.05)
            if os.pread(cfgf, 0x30, 0x10) != saved_hdr:                       # BARs lost in the reset
                os.pwrite(cfgf, saved_hdr, 0x10)
        os.pwrite(cfgf, (cmd_old | 0x2).to_bytes(2, 'little'), 4)            # memory decode on
        fd = os.open(f'{sysd}/resource0', os.O_RDWR | os.O_SYNC)
        mm = mmap.mmap(fd, BAR0_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        regs = (ctypes.c_uint32 * WORDS).from_buffer(mm)   # each element read = one 32-bit load
        words = [regs[i] for i in range(WORDS)]            # read only; nothing is written
        del regs
        mm.close(); os.close(fd)
    finally:
        os.pwrite(cfgf, cmd_old.to_bytes(2, 'little'), 4)
        os.close(cfgf)
        if pctl_old is not None and pctl_old != 'on':
            open(pctl, 'w').write(pctl_old)
    bad = sum(1 for x in words if x == 0xffffffff)
    if bad == WORDS:
        sys.exit('every read returned 0xffffffff - the card did not respond; nothing saved')
    d = outdir(args, 'snapshots')
    save_snapshot(d, 'cold-' + dt.datetime.now().strftime('%Y%m%d-%H%M%S'), words,
                  {'source': 'host sysfs resource0 (before guest driver)', 'time': now(),
                   'pmcsr_before': hex(pmcsr_old) if pmcsr_old is not None else None,
                   'power_control_before': pctl_old})


# ---------------------------------------------------------------- diff
def load(p):
    b = open(p, 'rb').read()
    return struct.unpack(f'<{len(b)//4}I', b)


def cmd_diff(args):
    a, b = load(args.a), load(args.b)
    ranges, cur = [], None
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            if cur and i - cur[1] <= 2:
                cur[1] = i
            else:
                cur = [i, i]; ranges.append(cur)
    print(f'{sum(1 for x, y in zip(a, b) if x != y)} words differ in {len(ranges)} ranges\n')
    for s, e in ranges:
        n = e - s + 1
        lab = label(s * 4)
        print(f'{s*4:05x}-{e*4:05x}  {n:5d} words  {lab}')
        if n <= args.show:
            for i in range(s, e + 1):
                if a[i] != b[i]:
                    print(f'      {i*4:05x}: {a[i]:08x} -> {b[i]:08x}')


# ---------------------------------------------------------------- decode (routes, SRC, DMA, mixer)
ADB_SRC = {**{i: 'ADBDMA%d' % i for i in range(0x20)}, **{0x20 + i: 'SRCOUT%d' % i for i in range(16)},
           **{0x30 + i: 'MIXOUT%d' % i for i in range(16)}, **{0x40 + i: 'WT0_%d' % i for i in range(0x26)},
           0x66: 'XTALKOUT0', 0x67: 'XTALKOUT1', **{0x68 + i: 'EFXOUT%d' % i for i in range(8)},
           **{0x70 + i: 'CODECIN%d' % i for i in range(4)}, 0x78: 'SPORTIN0', 0x79: 'SPORTIN1',
           0x7a: 'SPDIFIN0', 0x7b: 'SPDIFIN1', 0x7c: 'AC98IN0', 0x7d: 'AC98IN1', 0x7e: 'EQOUT0', 0x7f: 'EQOUT1',
           **{0x80 + i: 'WT1_%d' % i for i in range(0x26)}, **{0xa6 + i: 'A3DOUT%d' % i for i in range(8)}}
ADB_DST = {**{i: 'ADBDMA%d' % i for i in range(0x20)}, **{0x20 + i: 'ADBDMAB%d' % i for i in range(0x20)},
           **{0x40 + i: 'SRCIN%d' % i for i in range(16)}, **{0x50 + i: 'MIXIN%d' % i for i in range(0x20)},
           **{0x70 + i: 'A3DIN%d' % i for i in range(16)}, **{0x80 + i: 'EFXIN%d' % i for i in range(8)},
           **{0x88 + i: 'CODECOUT%d' % i for i in range(6)}, 0x90: 'SPORTOUT0', 0x91: 'SPORTOUT1',
           0x92: 'SPDIFOUT0', 0x93: 'SPDIFOUT1', 0x94: 'AC98OUT0', 0x95: 'AC98OUT1',
           **{0x96 + i: 'XTALKIN%d' % i for i in range(10)}, **{0xa0 + i: 'EQIN%d' % i for i in range(4)}}


def adb_routes(w):
    """Follow the linked route lists: CHNBASE[ch] -> RTBASE[route & 0xff] -> ... until 0xffff."""
    out = {}
    for ch in range(24):
        r, chain, seen = w[(0x282b4 >> 2) + ch] & 0xffff, [], set()
        while r != 0xffff and len(chain) < 64:
            if r in seen:                      # uninitialised table (power-on garbage) can loop
                chain.append(0xfffe)
                break
            seen.add(r)
            chain.append(r)
            r = w[(0x28000 >> 2) + (r & 0xff)] & 0xffff
        if chain:
            out[ch] = chain
    return out


def cmd_decode(args):
    w = load(args.snapshot)
    print('ADB routes (source -> sink), per ADB channel; ADB_SR = %08x' % w[0x28400 >> 2])
    for ch, chain in adb_routes(w).items():
        print('  ch%02d: ' % ch + ', '.join('(loop)' if r == 0xfffe else '%s->%s' % (
            ADB_SRC.get(r >> 8, hex(r >> 8)), ADB_DST.get(r & 0xff, hex(r & 0xff))) for r in chain))
    print('\nSample-rate converters (CONVRATIO: 0x4000 = 1.0 = 48 kHz in)')
    for i in range(16):
        cr = w[(0x26e40 >> 2) + i]
        if cr and (w[(0x26e00 >> 2) + i] & 0xfff0) == 0xc0:   # U0 low byte 0xcN = configured SRC
            print('  src%-2d CR=%05x -> %.1f Hz   U0=%08x U1=%08x U2=%08x U3=%08x' % (
                i, cr, cr * 48000 / 16384, w[(0x26e00 >> 2) + i], w[(0x26f00 >> 2) + i],
                w[(0x26f80 >> 2) + i], w[(0x26e80 >> 2) + i]))
    print('\nADB DMA (CTRL: bits 0-11 offset, 12 IRQ enable, 13 direction, 14-17 format)')
    for d in range(16):
        ctrl = w[(0x27a00 >> 2) + d]
        pages = [w[(0x27400 >> 2) + d * 4 + k] for k in range(4)]
        if (ctrl or any(pages)) and ctrl >> 18 == 0:          # higher bits set = never configured
            c0, c1 = w[(0x27800 >> 2) + d * 2], w[(0x27804 >> 2) + d * 2]
            print('  dma%-2d CTRL=%08x fmt=%x dir=%d ie=%d  CFG=%08x/%08x  pages=%s  STAT=%08x' % (
                d, ctrl, (ctrl >> 14) & 0xf, (ctrl >> 13) & 1, (ctrl >> 12) & 1, c0, c1,
                ' '.join('%08x' % x for x in pages), w[(0x27e00 >> 2) + d]))
    print('  ADBDMA_START=%08x  WTDMA_START=%08x' % (w[0x27c00 >> 2], w[0x27b00 >> 2]))
    print('\nMixer input volume targets (0x21000-0x217ff) that are not muted (0x80); layout not yet decoded')
    live = [(0x21000 + 4 * i, w[(0x21000 >> 2) + i]) for i in range(0x200) if w[(0x21000 >> 2) + i] & 0xff != 0x80]
    for i in range(0, len(live), 8):
        print('  ' + '  '.join('%05x=%02x' % (a, v & 0xff) for a, v in live[i:i + 8]))
    print('\nControl: CTRL=%08x IRQ_CTRL=%08x IRQ_SOURCE=%08x STAT=%08x' % tuple(
        w[a >> 2] for a in (0x2a00c, 0x2a004, 0x2a000, 0x2a008)))


# ---------------------------------------------------------------- clock (sample counter rate)
def cmd_clock(args):
    need_root()
    import time
    uid, gid, home = real_user()
    q = QMP(args.qmp or os.path.join(home, 'traces', 'qmp.sock'))
    base, _ = find_bar0(q)
    def rd():
        out = q.hmp(f'xp /1xw {base + 0x29198:#x}')
        return time.monotonic(), int(out.split(':')[1].split()[0], 16)
    t0, v0 = rd()
    time.sleep(args.seconds)
    t1, v1 = rd()
    rate = ((v1 - v0) & 0xffffffff) / (t1 - t0)
    print(f'SMP_TIME (0x29198) advanced {(v1 - v0) & 0xffffffff} in {t1 - t0:.4f} s = {rate:,.0f} Hz '
          f'({rate / 48000:.2f} x 48 kHz)')

# ---------------------------------------------------------------- codec
def cmd_codec(args):
    regs, idv, addr = {}, {}, None
    for line in open(args.timeline, errors='replace'):
        f = line.split()
        if len(f) < 5 or f[2] != '0x29188':
            continue
        v = int(f[4], 16)
        if f[1] == 'W':
            r = (v >> 16) & 0x7f
            if v & 0x800000:
                regs.setdefault(r, []).append(('W', v & 0xffff, f[0]))
            else:
                addr = r
        elif f[1] == 'R':
            r = (v >> 16) & 0x7f
            regs.setdefault(r, []).append(('R', v & 0xffff, f[0]))
    if not regs:
        sys.exit('no codec accesses (0x29188) found')
    vid = None
    if 0x7c in regs and 0x7e in regs:
        vid = (regs[0x7c][0][1] << 16) | regs[0x7e][-1][1]
        name = AC97_VENDORS.get(vid, 'unknown - look up in ALSA ac97_codec.c')
        print(f'AC97 codec ID {vid:08x}: {name}\n')
    print('reg   reads writes  first value  last value')
    for r in sorted(regs):
        acc = regs[r]
        rd = sum(1 for x in acc if x[0] == 'R'); wr = len(acc) - rd
        print(f'0x{r:02x}  {rd:5d} {wr:6d}      {acc[0][0]} {acc[0][1]:04x}       {acc[-1][0]} {acc[-1][1]:04x}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dev', default='0000:0c:01.0')
    ap.add_argument('--out')
    ap.add_argument('--qmp')
    sp = ap.add_subparsers(dest='cmd', required=True)
    sp.add_parser('pci')
    s = sp.add_parser('snap'); s.add_argument('name')
    sp.add_parser('cold')
    s = sp.add_parser('diff'); s.add_argument('a'); s.add_argument('b'); s.add_argument('--show', type=int, default=8)
    s = sp.add_parser('codec'); s.add_argument('timeline')
    s = sp.add_parser('decode'); s.add_argument('snapshot')
    s = sp.add_parser('clock'); s.add_argument('--seconds', type=float, default=2.0)
    args = ap.parse_args()
    {'pci': cmd_pci, 'snap': cmd_snap, 'cold': cmd_cold, 'diff': cmd_diff, 'codec': cmd_codec, 'decode': cmd_decode, 'clock': cmd_clock}[args.cmd](args)


if __name__ == '__main__':
    main()
