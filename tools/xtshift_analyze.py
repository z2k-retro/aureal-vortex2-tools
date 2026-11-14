#!/usr/bin/env python3
"""xtshift_analyze.py - levels per step of a 'vortex-poke.py xtshift' run.

  ./xtshift_analyze.py SESSION_DIR POKE_LOG [--offset SECONDS]

SESSION_DIR is the folder vortex-session.sh wrote (audio.wav and session.json).
POKE_LOG is the --log file vortex-poke.py wrote (only the latest xtshift run in it is used).

For every step it takes the clicks between 0.6 s after the step started and 0.3 s before
the next one, and reports per channel the median click peak (dBFS) and the RMS over the
window, with the change from the previous step of the same pass. If the shift is a plain
power of two, each step within a pass is +6.02 dB.

The recording's start time is only an estimate (session.json sample0_estimate); if the
steps look misaligned, nudge with --offset (positive = the audio is later than estimated).
"""
import array, datetime as dt, json, math, os, statistics, sys, wave


def ts(s):
    s = s.rstrip('Z')
    return dt.datetime.strptime(s, '%Y-%m-%dT%H:%M:%S.%f' if '.' in s else '%Y-%m-%dT%H:%M:%S')


def db(x):
    return 20 * math.log10(x) if x > 0 else float('-inf')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sess, logpath = sys.argv[1], sys.argv[2]
    offset = float(sys.argv[sys.argv.index('--offset') + 1]) if '--offset' in sys.argv else 0.0
    info = json.load(open(os.path.join(sess, 'session.json')))
    t0 = ts(info['recorder']['sample0_estimate'])
    w = wave.open(os.path.join(sess, 'audio.wav'))
    rate, nch = w.getframerate(), w.getnchannels()
    pcm = array.array('h', w.readframes(w.getnframes()))
    chans = [pcm[c::nch] for c in range(nch)]

    events = [json.loads(l) for l in open(logpath) if l.strip()]
    start = max((i for i, e in enumerate(events) if e.get('event') == 'xs_captured'), default=None)
    if start is None:
        sys.exit('no xtshift run in ' + logpath)
    run = events[start:]
    print('captured:', run[0].get('gains'))
    steps = [e for e in run if e.get('event') == 'xs_step']
    end = next((e for e in run if e.get('event') in ('xs_restored', 'interrupted')), None)
    times = [ts(e['utc']) for e in steps] + ([ts(end['utc'])] if end else [])

    prev = {}
    print(f"{'step':<44} " + '  '.join(f'ch{c} peak  d     rms ' for c in range(nch)))
    for i, e in enumerate(steps):
        if i + 1 >= len(times):
            break
        a = (times[i] - t0).total_seconds() + offset + 0.6
        b = (times[i + 1] - t0).total_seconds() + offset - 0.3
        lo, hi = int(a * rate), int(b * rate)
        if lo < 0 or hi > len(chans[0]) or hi <= lo:
            print(f'{e["name"]:<44} outside the recording'); continue
        cols = []
        group = e['name'][:1] if e['name'][:2] in ('A ', 'B ') else 'ref'
        for c in range(nch):
            seg = chans[c][lo:hi]
            # click peaks: the largest |sample| in each 250 ms slot (4 clicks per second)
            slot = rate // 4
            peaks = [max(abs(v) for v in seg[j:j + slot]) for j in range(0, len(seg) - slot + 1, slot)]
            pk = db(statistics.median(peaks) / 32768) if peaks else float('-inf')
            rms = db(math.sqrt(sum(v * v for v in seg) / len(seg)) / 32768)
            d = pk - prev[(group, c)] if (group, c) in prev and group != 'ref' else None
            prev[(group, c)] = pk
            clip = '!' if peaks and max(peaks) >= 32700 else ' '
            cols.append(f'{pk:7.2f}{clip}' + (f'{d:+6.2f}' if d is not None else '      ') + f' {rms:7.2f}')
        print(f'{e["name"]:<44} ' + '  '.join(cols))
    print('(! = a click reached full scale in that window; d = dB change from the previous step of the pass)')


if __name__ == '__main__':
    main()
