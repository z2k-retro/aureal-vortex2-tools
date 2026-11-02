"""Build an OpenAL Soft HRTF data set (MinPHR03) from the Aureal Vortex 2
direction table measured on an SQ2500: 56-tap HRIRs per ear at 48 kHz on a
15-degree grid (elevation -45..75 plus straight up) with separate ITDs."""
import csv, struct, sys
rows = list(csv.DictReader(open(sys.argv[1])))
tab = {}
for r in rows:
    az, el = int(r['az_deg']), int(r['el_deg'])
    L = [int(r[f'left{i}']) for i in range(56)]
    R = [int(r[f'right{i}']) for i in range(56)]
    tab[(az % 360, el)] = (L, R, float(r['itd_left_samples']), float(r['itd_right_samples']))
elevs = list(range(-90, 91, 15))          # 13 elevations, index 0 = -90
def lookup(az, el):
    if el >= 90:
        key = [k for k in tab if k[1] == 90][0]
        return tab[key]
    e = max(-45, min(75, el))             # the card's table stops at -45: clamp below
    return tab[(az % 360, e)]
out = bytearray(b'MinPHR03')
out += struct.pack('<IBBB', 48000, 1, 56, 1)   # rate, left+right, irSize, one field
out += struct.pack('<HB', 1000, len(elevs))
azc = [1 if abs(e) == 90 else 24 for e in elevs]
out += bytes(azc)
irs = []
for e, n in zip(elevs, azc):
    for k in range(n):
        irs.append(lookup(k * 360 // n, e))
for L, R, dl, dr in irs:
    for a, b in zip(L, R):
        for v in (a, b):
            out += struct.pack('<i', v * 256)[:3]
for L, R, dl, dr in irs:
    out += bytes([min(252, round(dl * 4)), min(252, round(dr * 4))])
open(sys.argv[2], 'wb').write(out)
print(len(irs), 'HRIRs,', len(out), 'bytes')
