#!/usr/bin/env bash
# vortex-session.sh - one Aureal Vortex 2 trace session:
#   records the card's analog output through the motherboard's audio input,
#   boots the Win98 QEMU VM with the Vortex passed through and register tracing on,
#   and keeps every timestamp in one place (UTC, same clock QEMU's trace uses).
#
# Run as your normal user (NOT with sudo). The script calls sudo only for QEMU,
# because PipeWire/PulseAudio recording has to run as you.
#
# Usage:
#   ./vortex-session.sh list-inputs            show recording devices
#   ./vortex-session.sh test-rec [SECONDS]     record a few seconds, report levels
#   ./vortex-session.sh run LABEL              full session (record + VM + trace)
#   ./vortex-session.sh mark "TEXT"            (from a 2nd terminal) timestamp a marker
#
# Settings (environment variables, defaults in brackets):
#   REC_TARGET   PipeWire source name/ID for pw-record, or ALSA device for arecord [system default]
#   REC_RATE     sample rate [48000]
#   REC_PORT     input jack to select first via pactl, e.g. analog-input-linein [leave as is]
#   MIXER_CARD   ALSA card whose line input gain is set before recording [0]; MIXER_CARD=none skips
#   CAPTURE_DB   hardware Capture gain in dB [0]  (Line Boost is always forced to 0)
#   ACCEL        kvm or tcg [kvm]
#   QEMU         [~/qemu/build/qemu-system-i386]
#   DISK         [~/win98-qemu/win98.qcow2]
#   HOSTDEV      [0000:0c:01.0]
#   EXTRA_CD     ISO to attach as the CD-ROM [none]
#   EXTRA_DISK   raw disk image attached as a second hard disk that KEEPS its changes
#                even with -snapshot (used for a3dtest results) [none]
#   TRACES       session folders go here [~/traces]
#   PARSER       vortex_trace.py, run on the log afterwards [../tools/vortex_trace.py]
#   PERSIST=1    keep disk changes (drops -snapshot) [off]
#   NOKVMINTX=1  deliver the card's interrupt through QEMU instead of KVM's fast path
#                (x-no-kvm-intx=on): slower, but use it if the driver stops getting interrupts [off]
#   VGA          cirrus; std (QEMU standard VGA, for SoftGPU); or voodoo3 for the
#                emulated Voodoo3 (needs a QEMU built with the QEmu-3Dfx-Voodoo-3-Port
#                device, set with QEMU=) [cirrus]
#   V3ROM        Voodoo3 3000 video BIOS for VGA=voodoo3 [~/roms/3k12sd.rom]

set -u

REC_RATE=${REC_RATE:-48000}
ACCEL=${ACCEL:-kvm}
QEMU=${QEMU:-$HOME/qemu/build/qemu-system-i386}
DISK=${DISK:-$HOME/win98-qemu/win98.qcow2}
HOSTDEV=${HOSTDEV:-0000:0c:01.0}
EXTRA_CD=${EXTRA_CD:-}
TRACES=${TRACES:-$HOME/traces}
if [ -z "${PARSER:-}" ]; then
    # next to the repo's tools/, or a clone in the home folder when this script is a copy
    for p in "$(dirname "$0")/../tools/vortex_trace.py" "$(dirname "$0")/vortex_trace.py" \
             "$HOME/aureal-vortex2-tools/tools/vortex_trace.py"; do
        [ -f "$p" ] && { PARSER=$p; break; }
    done
    PARSER=${PARSER:-$HOME/aureal-vortex2-tools/tools/vortex_trace.py}
fi
PERSIST=${PERSIST:-0}
VGA=${VGA:-cirrus}
V3ROM=${V3ROM:-$HOME/roms/3k12sd.rom}
CURRENT_LINK="$TRACES/current"

die()  { echo "ERROR: $*" >&2; exit 1; }
note() { echo ">> $*"; }
now()  { date -u +%Y-%m-%dT%H:%M:%S.%6NZ; }      # same format as QEMU's trace lines

rec_tool() {
    if command -v pw-record >/dev/null 2>&1; then echo pw-record
    elif command -v arecord >/dev/null 2>&1; then echo arecord
    else die "neither pw-record nor arecord found (sudo apt install pipewire-bin alsa-utils)"; fi
}

# start_recorder OUTFILE -> sets REC_PID
select_port() {
    [ -n "${REC_PORT:-}" ] || return 0
    command -v pactl >/dev/null 2>&1 || die "REC_PORT set but pactl not found"
    local src; src=$(pactl get-default-source)
    pactl set-source-port "$src" "$REC_PORT" || die "could not set $src to port $REC_PORT (see: pactl list sources)"
    note "input $src set to port $REC_PORT"
}

# Force sane line-input gain: Input Source=Line, Line Boost 0, Capture CAPTURE_DB.
# (On the ALC892 these had been at +30 dB each, which pinned the recording at full scale.)
set_mixer() {
    local card=${MIXER_CARD:-0} db=${CAPTURE_DB:-0}
    [ "$card" = none ] && return 0
    command -v amixer >/dev/null 2>&1 || die "amixer not found (sudo apt install alsa-utils), or set MIXER_CARD=none"
    amixer -q -c "$card" sset 'Input Source' Line 2>/dev/null
    amixer -q -c "$card" sset 'Line Boost' 0       || die "could not set Line Boost on card $card"
    amixer -q -c "$card" sset Capture "${db}dB" cap || die "could not set Capture on card $card"
    local got; got=$(amixer -c "$card" sget Capture | grep -o '\[-\?[0-9.]*dB\]' | head -1 | tr -d '[]dB')
    local boost; boost=$(amixer -c "$card" sget 'Line Boost' | grep -o '\[-\?[0-9.]*dB\]' | head -1 | tr -d '[]dB')
    python3 -I -c "import sys; sys.exit(0 if abs(float('$got')-float('$db'))<0.6 and abs(float('${boost:-0}'))<0.1 else 1)" \
        || die "mixer did not stick (Capture ${got}dB, Line Boost ${boost}dB) - PipeWire may be overriding it"
    note "line input: Capture ${got} dB, Line Boost ${boost} dB"
}

start_recorder() {
    local out=$1 tool; tool=$(rec_tool)
    if [ "$tool" = pw-record ]; then
        local tgt=(); [ -n "${REC_TARGET:-}" ] && tgt=(--target "$REC_TARGET")
        setsid pw-record "${tgt[@]}" --rate "$REC_RATE" --channels 2 --format s16 "$out" &
    else
        local dev=(); [ -n "${REC_TARGET:-}" ] && dev=(-D "$REC_TARGET")
        setsid arecord "${dev[@]}" -q -f S16_LE -c 2 -r "$REC_RATE" -t wav "$out" &
    fi
    REC_PID=$!
}

stop_recorder() {
    [ -n "${REC_PID:-}" ] || return 0
    kill -INT "$REC_PID" 2>/dev/null
    wait "$REC_PID" 2>/dev/null
    REC_PID=
}

# Peak and RMS per channel, in dBFS, plus length
wav_report() {
    python3 -I - "$1" <<'EOF'
import sys, wave, array, math
w = wave.open(sys.argv[1])
ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
a = array.array('h', w.readframes(n))
print(f"   {n/rate:.3f} s at {rate} Hz, {ch} ch")
for c in range(ch):
    s = a[c::ch]
    if not s: continue
    pk = max(abs(x) for x in s); rms = math.sqrt(sum(x*x for x in s)/len(s))
    db = lambda v: 20*math.log10(v/32768) if v else float('-inf')
    clip = sum(1 for x in s if abs(x) >= 32767)
    print(f"   ch{c}: peak {db(pk):6.1f} dBFS  rms {db(rms):6.1f} dBFS  clipped samples {clip}")
if ch == 2 and n:
    # stereo check on first differences (drops DC and low-frequency drift), 10% of the file
    L = a[0::2]; R = a[1::2]; blk = 4800; sm = ss = 0.0
    for start in range(0, len(L) - blk, blk * 10):
        for i in range(start + 1, start + blk):
            dl = L[i] - L[i-1]; dr = R[i] - R[i-1]
            sm += (dl + dr) ** 2; ss += (dl - dr) ** 2
    if sm == 0 and ss == 0:
        print("   stereo check: silent")
    elif sm == 0:
        print("   stereo check: channels cancel (one inverted?)")
    else:
        r = 10 * math.log10(ss / sm) if ss else float('-inf')
        msg = f"   side/mid {r:6.1f} dB"
        if r < -30:
            msg += "  <-- WARNING: left and right nearly identical (mono input or mono cable?)"
        elif r > -1 and r < 1 and min(sum(x*x for x in L[:48000]), sum(x*x for x in R[:48000])) == 0:
            msg += "  <-- WARNING: one channel silent"
        else:
            msg += "  (a sound panned hard to one side should push this toward 0 dB)"
        print(msg)
EOF
}

cmd_list_inputs() {
    if command -v wpctl >/dev/null 2>&1; then
        note "PipeWire sources (use the ID or node name as REC_TARGET):"
        wpctl status | sed -n '/Sources:/,/^ *$/p'
    fi
    if command -v arecord >/dev/null 2>&1; then
        note "ALSA capture hardware:"
        arecord -l
    fi
}

cmd_test_rec() {
    local secs=${1:-5} f; f=$(mktemp --suffix=.wav)
    select_port
    set_mixer
    note "Recording $secs s with $(rec_tool) - play something on the Vortex now"
    start_recorder "$f"; sleep "$secs"; stop_recorder
    wav_report "$f"
    echo "   Aim for peaks around -12 to -3 dBFS and zero clipped samples."
    echo "   File kept at $f"
}

cmd_mark() {
    [ -L "$CURRENT_LINK" ] || die "no session running"
    local t; t=$(now)
    printf '%s\t%s\n' "$t" "${*:-mark}" >> "$CURRENT_LINK/markers.tsv"
    note "marked $t  ${*:-mark}"
}

preflight() {
    [ "$(id -u)" -ne 0 ] || die "run as your normal user, not with sudo"
    [ -x "$QEMU" ] || die "QEMU not found at $QEMU"
    [ -f "$DISK" ] || die "disk not found at $DISK"
    local drv=""
    [ -e "/sys/bus/pci/devices/$HOSTDEV/driver" ] && drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$HOSTDEV/driver")")
    [ "$drv" = vfio-pci ] || die "$HOSTDEV is bound to '${drv:-nothing}', not vfio-pci (sudo driverctl set-override $HOSTDEV vfio-pci)"
    [ -z "$EXTRA_CD" ] || [ -f "$EXTRA_CD" ] || die "EXTRA_CD not found: $EXTRA_CD"
    case "$VGA" in
        cirrus|std) ;;
        voodoo3) [ -f "$V3ROM" ] || die "Voodoo3 BIOS not found at $V3ROM (set V3ROM=...)"
                 "$QEMU" -device help 2>/dev/null | grep -q '"voodoo3"' \
                     || die "$QEMU has no voodoo3 device (set QEMU= to the Voodoo3 build)" ;;
        *) die "VGA must be cirrus, std or voodoo3" ;;
    esac
    [ -z "${EXTRA_DISK:-}" ] || [ -f "$EXTRA_DISK" ] || die "EXTRA_DISK not found: $EXTRA_DISK"
    rec_tool >/dev/null
    if pgrep -af 'qemu-system' >/dev/null; then
        echo "ERROR: a QEMU is already running and probably holds the Vortex:" >&2
        pgrep -af 'qemu-system' >&2
        die "shut that VM down first (or: sudo pkill -f qemu-system)"
    fi
    note "sudo is needed for QEMU; caching your password now"
    sudo -v || die "sudo failed"
}

cmd_run() {
    local label=${1:-session}
    label=${label//[^A-Za-z0-9._-]/_}
    preflight
    local stamp dir; stamp=$(date +%Y%m%d-%H%M%S)
    dir="$TRACES/$stamp-$label"; mkdir -p "$dir" || die "cannot create $dir"
    ln -sfn "$dir" "$CURRENT_LINK"
    : > "$dir/markers.tsv"
    sudo rm -f "$TRACES/qmp.sock"

    local args=(
        -machine pc,hpet=off -accel "$ACCEL" -cpu pentium3 -m 256
        -rtc base=localtime -display sdl -nic none
        -drive "file=$DISK,format=qcow2,if=ide"
        -device "vfio-pci,host=$HOSTDEV,x-no-mmap=on${NOKVMINTX:+,x-no-kvm-intx=on}"
        -trace vfio_region_write -trace vfio_region_read
        -msg timestamp=on -D "$dir/qemu-trace.log"
        -monitor stdio
        -qmp "unix:$TRACES/qmp.sock,server=on,wait=off"
    )
    # Fixed PCI slot for the Voodoo3, so Windows sees the same device every run
    if [ "$VGA" = voodoo3 ]; then
        args+=(-vga none -device "voodoo3,model=3,legacy-vga=on,romfile=$V3ROM,addr=0x05")
    else
        args+=(-vga "$VGA")
    fi
    [ "$PERSIST" = 1 ] || args+=(-snapshot)
    [ -n "$EXTRA_CD" ] && args+=(-cdrom "$EXTRA_CD")
    [ -n "${EXTRA_DISK:-}" ] && args+=(-drive "file=$EXTRA_DISK,format=raw,if=ide,index=1,snapshot=off")

    note "session folder: $dir"
    select_port
    set_mixer
    [ "${MIXER_CARD:-0}" = none ] || amixer -c "${MIXER_CARD:-0}" scontents > "$dir/mixer.txt" 2>/dev/null
    note "starting recorder ($(rec_tool), $REC_RATE Hz)"
    local rec_before rec_after
    rec_before=$(now); start_recorder "$dir/audio.wav"; rec_after=$(now)
    sleep 1
    kill -0 "$REC_PID" 2>/dev/null || die "recorder exited immediately - check REC_TARGET (./vortex-session.sh list-inputs)"

    trap 'stop_recorder' EXIT INT TERM
    note "starting QEMU. End the session by shutting Windows down (or typing quit at the (qemu) prompt)."
    note "Markers, from a second terminal: $0 mark \"what just happened\""
    local qemu_start qemu_end
    qemu_start=$(now)
    # Pass only the display variables through: sudo-rs (newer Ubuntu) ignores -E
    sudo env DISPLAY="${DISPLAY:-}" XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        "$QEMU" "${args[@]}"
    local qrc=$?
    qemu_end=$(now)
    if [ "$qrc" -ne 0 ]; then
        echo "WARNING: QEMU exited with code $qrc - this session is probably not usable" >&2
    fi
    sleep 1                      # catch the tail of the audio
    local rec_stop; rec_stop=$(now)
    stop_recorder
    trap - EXIT INT TERM
    rm -f "$CURRENT_LINK"

    sudo chown "$(id -u):$(id -g)" "$dir"/* 2>/dev/null

    python3 -I - "$dir" "$rec_before" "$rec_after" "$qemu_start" "$qemu_end" "$rec_stop" "$qrc" \
        "$(rec_tool)" "${REC_TARGET:-default}" "$REC_RATE" "$ACCEL" "$PERSIST" "$QEMU" "${args[*]}" <<'EOF'
import sys, json, wave, os, datetime as dt
d, rb, ra, qs, qe, rs, qrc, tool, tgt, rate, accel, persist, qemu, args = sys.argv[1:]
P = lambda s: dt.datetime.strptime(s, '%Y-%m-%dT%H:%M:%S.%fZ')
info = {
  "clock": "all times UTC, same clock and format as qemu-trace.log",
  "recorder": {"tool": tool, "target": tgt, "rate": int(rate),
               "spawn_before": rb, "spawn_after": ra, "stop": rs},
  "qemu": {"start": qs, "exit": qe, "exit_code": int(qrc), "accel": accel,
           "snapshot_mode": persist != "1", "binary": qemu, "args": args},
}
try:
    w = wave.open(os.path.join(d, "audio.wav"))
    secs = w.getnframes() / w.getframerate()
    wall = (P(rs) - P(rb)).total_seconds()
    info["recorder"]["audio_seconds"] = round(secs, 6)
    info["recorder"]["wallclock_seconds"] = round(wall, 6)
    # Estimated time of sample 0: assume the recording ends at 'stop' and runs at nominal rate.
    info["recorder"]["sample0_estimate"] = (P(rs) - dt.timedelta(seconds=secs)).strftime('%Y-%m-%dT%H:%M:%S.%fZ')
    info["recorder"]["start_uncertainty_note"] = (
        "sample0 lies between spawn_before and roughly sample0_estimate; "
        "refine by matching the first audible sound to its register activity")
except Exception as e:
    info["recorder"]["error"] = str(e)
def trace_span(path):
    first = last = None
    try:
        with open(path, errors="replace") as f:
            for line in f:
                if "vfio_region" in line:
                    first = line.split()[0]; break
        with open(path, "rb") as f:
            f.seek(0, 2); f.seek(max(0, f.tell() - 65536))
            for line in f.read().decode(errors="replace").splitlines():
                if "vfio_region" in line: last = line.split()[0]
    except OSError:
        pass
    return first, last
def P2(s):
    s = s.rstrip("Z")
    return dt.datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%f" if "." in s else "%Y-%m-%dT%H:%M:%S")
tf, tl = trace_span(os.path.join(d, "qemu-trace.log"))
info["trace"] = {"first_access": tf, "last_access": tl,
                 "note": "timeline.txt time 0 = first_access"}
if tf and "sample0_estimate" in info["recorder"]:
    s0 = P2(info["recorder"]["sample0_estimate"])
    info["trace"]["audio_seconds_at_trace_t0"] = round((P2(tf) - s0).total_seconds(), 6)
markers = [l.rstrip("\n").split("\t", 1) for l in open(os.path.join(d, "markers.tsv")) if l.strip()]
info["markers"] = [{"time": m[0], "text": m[1] if len(m) > 1 else ""} for m in markers]
json.dump(info, open(os.path.join(d, "session.json"), "w"), indent=2)
print(json.dumps(info["recorder"], indent=2))
EOF

    if [ -n "${EXTRA_DISK:-}" ] && command -v mcopy >/dev/null 2>&1; then
        if MTOOLS_SKIP_CHECK=1 mcopy -o -i "$EXTRA_DISK@@1M" ::A3DTEST.LOG "$dir/a3dtest.log" 2>/dev/null; then
            note "copied A3DTEST.LOG from the results disk"
        fi
    fi
    note "audio levels:"; wav_report "$dir/audio.wav"
    if [ -f "$PARSER" ]; then
        note "building register timeline"
        python3 -I "$PARSER" timeline "$dir/qemu-trace.log" "$dir/timeline.txt" && \
        python3 -I "$PARSER" summary  "$dir/qemu-trace.log" > "$dir/summary.txt"
    else
        note "parser not found at $PARSER - skipping timeline (set PARSER=...)"
    fi
    note "done. To share: cd $TRACES && zip -r $stamp-$label.zip $stamp-$label"
}

case "${1:-}" in
    list-inputs) cmd_list_inputs ;;
    test-rec)    shift; cmd_test_rec "$@" ;;
    run)         shift; cmd_run "$@" ;;
    mark)        shift; cmd_mark "$@" ;;
    *) sed -n '2,30p' "$0" ;;
esac
