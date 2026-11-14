"""Build an OpenAL Soft HRTF data set (MinPHR03) for the A3D DLLs from
Aureal's own HRTF profile file, "Ed's Ears" (Ed_Riegelsberger1.hrtf), which
ships with the Vortex 2 drivers and is the direction table the 4.06.2048
driver loads into the card. The result, "Aureal Vortex 2.mhr", is kept
in this repository as data/hrtf/"Aureal Vortex 2.mhr".

    python3 make_mhr.py Ed_Riegelsberger1.hrtf "Aureal Vortex 2.mhr"

The file holds, per copy, 267 filters of 56 signed 16-bit taps for the left
ear, then 267 for the right: 24 azimuths x 11 elevations (15-degree steps,
-75 to +75), then straight up, straight down and a unit impulse. Filter
index = ((12 - azimuth/15) mod 24) * 11 + (5 - elevation/15), with azimuth
counted as in the card's tables (90 = the listener's left: the left ear
leads). After the taps come a count word (534), a zero word, then 267 left
and 267 right ITDs in 1/256-sample units, in the same order.

The old capture-based route still works if given a .csv of the measured
table (az_deg, el_deg, left0..55, right0..55, itd_left_samples,
itd_right_samples)."""
import csv, struct, sys

N, TAPS = 267, 56


def read_hrtf(path):
    d = open(path, 'rb').read()
    if d[:4] != b'HRTF':
        sys.exit('not an Aureal .hrtf file')
    rate = struct.unpack_from('<I', d, 8)[0]
    namelen = struct.unpack_from('<I', d, 0x34)[0]
    o = 0x38 + namelen
    authlen = struct.unpack_from('<I', d, o)[0]
    o += 4 + authlen
    o += 4                              # block length word (29904)
    blk = N * TAPS * 2
    L = struct.unpack_from('<%dh' % (N * TAPS), d, o)
    R = struct.unpack_from('<%dh' % (N * TAPS), d, o + blk)
    itd = struct.unpack_from('<%dh' % (2 + 2 * N), d, o + 2 * blk)
    tab = {}
    def put(key, i):
        tab[key] = (list(L[i * TAPS:(i + 1) * TAPS]), list(R[i * TAPS:(i + 1) * TAPS]),
                    itd[2 + i] / 256.0, itd[2 + N + i] / 256.0)
    for ai in range(24):
        for ei in range(-5, 6):
            put((ai * 15, ei * 15), ((12 - ai) % 24) * 11 + 5 - ei)
    put('up', 264)
    put('down', 265)
    return rate, tab


def read_csv(path):
    tab = {}
    for r in csv.DictReader(open(path)):
        az, el = int(r['az_deg']) % 360, int(r['el_deg'])
        v = ([int(r[f'left{i}']) for i in range(TAPS)], [int(r[f'right{i}']) for i in range(TAPS)],
             float(r['itd_left_samples']), float(r['itd_right_samples']))
        tab['up' if el >= 90 else (az, el)] = v
    return 48000, tab


rate, tab = (read_csv if sys.argv[1].lower().endswith('.csv') else read_hrtf)(sys.argv[1])
lo = min(k[1] for k in tab if k not in ('up', 'down'))
hi = max(k[1] for k in tab if k not in ('up', 'down'))


def lookup(az, el):
    if el >= 90:
        return tab['up']
    if el <= -90:
        return tab.get('down', tab[(az % 360, lo)])
    return tab[(az % 360, max(lo, min(hi, el)))]


elevs = list(range(-90, 91, 15))          # 13 elevations, index 0 = -90
out = bytearray(b'MinPHR03')
out += struct.pack('<IBBB', rate, 1, TAPS, 1)   # rate, left+right, irSize, one field
out += struct.pack('<HB', 1000, len(elevs))
azc = [1 if abs(e) == 90 else 24 for e in elevs]
out += bytes(azc)
irs = [lookup(k * 360 // n, e) for e, n in zip(elevs, azc) for k in range(n)]
for L, R, dl, dr in irs:
    for a, b in zip(L, R):
        for v in (a, b):
            out += struct.pack('<i', v * 256)[:3]
for L, R, dl, dr in irs:
    out += bytes([min(252, round(dl * 4)), min(252, round(dr * 4))])
open(sys.argv[2], 'wb').write(out)
print(len(irs), 'HRIRs from', sys.argv[1], '(elevations %d..%d%s),' % (lo, hi, ', down' if 'down' in tab else ''), len(out), 'bytes')
