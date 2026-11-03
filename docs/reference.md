# Aureal Vortex 2 (AU8830) hardware reference

Last updated 2026-11-01 · Zvi Rosen ([z2k-retro](https://github.com/z2k-retro)) · see [the appendices](README.md) for data, formats and procedures

## Purpose and status

This reference records what a real Aureal Vortex 2 card does at the register level, so that anyone can build an emulated card or a replacement driver without starting from scratch. The [86Box device](using-86box.md) was built from it. The headline finding: **Aureal's own Windows driver computes the HRTF filters and sends finished coefficients to the chip**, so an emulator needs to run fixed filters, not reproduce Aureal's 3D math.

Evidence comes from one card, observed through the stock Windows 98 driver:

| Item | Detail |
| --- | --- |
| Card | Aureal SQ2500 (AU8830 "Vortex 2"), PCI revision `fa` |
| Driver | Aureal Vortex driver 4.06.2048 under Windows 98 SE |
| Host | ASUS Sabertooth X79 (BIOS 4701), card in the chipset's legacy PCI slot |
| Capture | QEMU 11.1.2 with VFIO passthrough, every register access logged; card output recorded through the host line-in |

Each finding is tagged by how well it is established:

- **Confirmed**: seen directly and consistent across traces, snapshots or recorded audio.
- **Strong**: one clear line of evidence, not yet tested independently.
- **Hypothesis**: a reading that fits the data but needs a targeted test.

Register names marked "(ALSA)" come from the Linux `au88x0` driver; everything else is from our own observations. Session names (for example `20261031-142309-a3dtest2`) and "run" numbers refer to the measurement sessions listed at the end; the procedures are in [Appendix C](appendix-c-workflow.md).

## Method

The real card is handed to a Windows 98 virtual machine, and QEMU logs every read and write the stock driver makes to it, with timestamps. Nothing on the card is emulated; the driver talks to real silicon.

1. **Passthrough.** The card is bound to `vfio-pci` on the Linux host and passed to a QEMU `pc` (440FX) machine running Windows 98 SE.
2. **Trapping every access.** The device is added with `-device vfio-pci,host=…,x-no-mmap=on`. With `x-no-mmap` on, the guest cannot write to the card directly, so every BAR access goes through QEMU and is logged by the `vfio_region_read` and `vfio_region_write` trace events (`-trace … -msg timestamp=on`).
3. **Sessions.** `scripts/vortex-session.sh` starts an audio recording of the card's line-out, boots the VM with tracing on, and stores all start and stop times in UTC on the same clock QEMU uses. Trace and audio line up to well under a second; the Windows startup sound and each playback period appear in both.
4. **Register snapshots.** `vortexprobe.py snap` reads the whole 256 KB register window from the running VM through QEMU's QMP `xp` command. `vortexprobe.py cold` reads the same window from Linux after a cold boot, before any driver has touched the card.
5. **Analysis.** `vortex_trace.py` turns raw logs into a register timeline; `a3d_params.py` extracts the per-source A3D data described below.

Test content was first the A3D demos shipped with the driver package (a helicopter and a fountain moved around the listener, toward and away from them, and played as WAV sources in the player demo), then `a3dtest`, a test program in this repository that drives the A3D and DirectSound APIs one setting at a time and logs each step, and finally games (Half-Life, Unreal, Duke Nukem 3D's setup).

## PCI identity

The card is a plain PCI 2.x audio device with one 256 KB memory BAR, two 8-byte I/O BARs, no option ROM and one power-management capability (Confirmed, `lspci -xxxx` and a raw `config` read).

| Field | Offset | Value |
| --- | --- | --- |
| Vendor / device | 0x00 | `12eb` / `0002` (Aureal, Vortex 2) |
| Command / status at power-on | 0x04 | `0000` / `0290` (capability list, fast back-to-back, DEVSEL medium) |
| Revision | 0x08 | `fa` |
| Class | 0x09 | `040100` (multimedia audio) |
| Cache line / latency timer | 0x0c | `10` / `20` (host-assigned) |
| Header type | 0x0e | `00` |
| BAR0 | 0x10 | 256 KB memory, 32-bit, non-prefetchable |
| BAR1 | 0x14 | 8 bytes I/O |
| BAR2 | 0x18 | 8 bytes I/O |
| Subsystem vendor / ID | 0x2c | `12eb` / `0002` |
| Expansion ROM | 0x30 | none (`00000000`) |
| Capability pointer | 0x34 | `dc` |
| Interrupt pin | 0x3d | A |
| Min\_Gnt / Max\_Lat | 0x3e | `04` / `0c` |
| Vendor-specific | 0x40 | `80`; ALSA writes 0xff here as a PCI latency workaround for some VIA and AMD chipsets |
| Power management | 0xdc | PM version 1.0, DSI set, D2 supported, D1 not; `NoSoftRst-` |

`NoSoftRst-` means a D3 to D0 transition resets the chip's internal state. Host software that idles the card in D3, as VFIO does, therefore hands the guest a freshly reset card.

## Host interface

The chip's registers form one 32-bit address space of 256 KB, reachable two ways: directly through BAR0, or one register at a time through an index/data pair in BAR1 (Confirmed). The same register offsets are used either way.

| Path | How it works | When the Windows 98 driver uses it |
| --- | --- | --- |
| BAR0 (memory, 256 KB) | Register at offset X is BAR0 + X; 32-bit accesses | Everything once Windows is running, including all A3D traffic |
| BAR1 (I/O, 8 bytes) | Write the register offset to BAR1+0, then read or write the value at BAR1+4 (32-bit) | Early initialization only: the first \~20 s of boot, about 1,250 accesses, then never again |
| BAR2 (I/O, 8 bytes) | Byte accesses | Four accesses in a whole session: write `0x10` to +4 twice, read +6 (`0xff`), write `0x00` to +4 |

The BAR1 window matters for an emulator: the first thing the driver does is write `0x29850` to BAR1+0 and read BAR1+4 (`0x302` on this card). BAR2's purpose is unknown (Hypothesis: a legacy-compatibility port block such as MIDI or joystick).

## Address map

The chip implements registers from `0x00000` to `0x2b44f`; the remaining 80 KB of the 256 KB window is either empty or not decoded at all. Block names in the map below come from the Linux ALSA driver headers; reset contents and activity are our own observations. The map combines a cold read taken after power-on reset with snapshots of the running driver at idle, during WAV playback and during the A3D demo. The reset contents come from a read after a full power-off (power cord out). A warm reboot resets the control registers but leaves memory-like regions (route table, converter and DMA settings, filter memory) holding the previous session's values; after a true power-off those regions hold random data, and the driver initializes them itself. Control-register reset values were identical in both reads, and every register the driver reads before ever writing, and that does not change on its own, matched it (9 of 9). In the table, contents listed for memory regions (wavetable, FIFO data, A3D filter memory, mixer, converters, DMA) are as found after a warm reboot; after a power-off they are random, apart from the fixed patterns.

| Range | Block (ALSA names) | Reset contents | Driver activity observed |
| --- | --- | --- | --- |
| `00000–0ffff` | Wavetable (WT) engine, 64 voices in two halves `0x8000` apart. Per voice: `PARM0–3` at `0x200–0x20c`, `DELAY` at `0x400–0x40c`; block control (`CTRL`, ramps) | Every 2 KB: four words at +0x00/+0x20/+0x40/+0x60 = `0x3e`; rest 0 | Driver writes `CTRL` = `0x1aa3` (ALSA uses `0x1aa2`). WT voice 0 parameters carry A3D 2.0 reflections (see A3D engine) |
| `10000–13fff` | `FIFO_WTDATA` | 0 | WT audio data during playback |
| `14000–15fff` | `FIFO_ADBDATA` | 0 | ADB audio data during playback |
| `16000–16fff` | `FIFO_WTCTRL` `0x16000`, `FIFO_ADBCTRL` `0x16100` | Mostly `0x000DDABA` | Rewritten whenever a sound buffer is restarted |
| `17000–17fff` | `FIFO_GIRT` | `0x000DDABA` | A few words at setup |
| `18000–1ffff` | A3D: 4 slices of `0x2000`, 4 sources each (see A3D engine) | Implemented words 0; gaps `0xDEADBABE` | Rewritten continuously during A3D only |
| `20000–21fff` | Mixer: input volume current `0x20000` / target `0x21000`, output volume current `0x20800` / target `0x21800`; `MIX_ENIN` `0x21a00`, `MIX_SMP` `0x21c00`, routes `0x21e00`, `MIXER_SR` `0x21f00`, `MIXER_CLIP` `0x21f80` | Half `0x80` (= `VOL_MIN`, muted), half `0x002DDEAD` | In Stereo mode, input volumes at `0x202xx`/`0x212xx` ramp continuously (panning) |
| `22000–23fff` | Includes `SPDIF_FLAGS` `0x2205c` | `0xff`, `0x00ffffff` | Set at startup |
| `24000–25fff` | Crosstalk canceller: EQ and cross-talk coefficient sets from `0x24200` (ALSA `au88x0_xtalk.c`) | Partly `0xDEADBABE` | Almost idle until A3D starts, then \~235 words per page change |
| `26000–26fff` | Sample-rate converters: data `0x26000`/`0x26800`, routes `0x26c00`, parameters `0x26e00–0x26f80` (`CONVRATIO` `0x26e40`) | Mostly 0 | Heavy during WAV and A3D |
| `27000–27fff` | DMA engines: WT DMA (`BUFBASE` `0x27000`, `CTRL` `0x27900`, `START` `0x27b00`, `STAT` `0x27d00`), ADB DMA (`BUFBASE` `0x27400`, `CTRL` `0x27a00`, `START` `0x27c00`, `STAT` `0x27e00`), `ENGINE_CTRL` `0x27ae8` | Mixed, includes `0xDEADBEEF` | Status registers read constantly during playback |
| `28000–28fff` | ADB route table `0x28000`, `ADB_SR` `0x28400`, MIDI `0x28800`, game port `0x28808–0x2881c` | Mixed | Heavy setup at startup |
| `29000–29fff` | Codec `0x29184–0x29190`, `SMP_TIME` `0x29198` (sample counter), `IRQ_STAT` / timer `0x2919c`, `SPDIF_SMPRATE` `0x29194`, S/PDIF status `0x291d0` | Mixed | `0x29198` polled constantly, counts upward |
| `2a000–2afff` | `IRQ_SOURCE` `0x2a000`, `IRQ_CTRL` (mask) `0x2a004`, `STAT` `0x2a008`, `CTRL` `0x2a00c` | 256 words of `0xDEADDEAD` | Read and written on every interrupt |
| `2b000–2b44f` | Hardware EQ, base `0x2b000` (`au88x0_eq.c`) | Mixed | Light |
| `2b450–2bfff` | Decoded but empty | `0xDEADBABE`, always | None |
| `2c000–3ffff` | Not decoded | Returns the last value the card put on the bus | None |

The last row matters for accuracy (Strong). In running snapshots, each 4 KB read chunk in this range came back as one repeated value, and each value matched a register the driver had just read, such as interrupt status `0x30024` or the counter at `0x29198`. In the cold read it looked like `0xDEADBABE` only because the last real read before it was in the `0xDEADBABE` range.

The repeated filler values (`0xDEADBABE`, `0xDEADBEEF`, `0xDEADDEAD`, `0x000DDABA`, `0x002DDEAD`) come back identically after a full power-off, while the surrounding memory is random, so they are fixed read-back values wired into the chip rather than memory contents. An emulator should return them as constants.

Selected reset values of registers the driver uses early:

| Register | Reset value |
| --- | --- |
| `0x29184` (`CODEC_CTRL`) | `0x00008100` |
| `0x29194` (`SPDIF_SMPRATE`) | `0x80000000` |
| `0x29850` | `0x00000302` |
| `0x2a000` (`IRQ_SOURCE`) | `0x00000004` |
| `0x2a00c` (`CTRL`) | `0x00303260` |

## AC'97 codec

The SQ2500's analog side is a SigmaTel STAC9708/9711, AC'97 vendor ID `0x83847608` (Confirmed: read by the driver from codec registers `0x7c` = `0x8384` and `0x7e` = `0x7608`).

The driver reaches the codec through `CODEC_IO` at `0x29188` (ALSA):

| Bits | Meaning |
| --- | --- |
| 23 | 1 = write, 0 = read request |
| 22–16 | AC'97 register number |
| 15–0 | Data |

A read is a write of the register number with bit 23 clear, followed by a read of `0x29188`; the returned word repeats the register number in bits 22–16 with the data below.

Startup sequence, in order:

1. Write register `0x00` (reset); read the vendor ID at `0x7c` and `0x7e`.
2. Write SigmaTel-specific registers: `0x76` = `0xabba`, `0x78` = `0x1000`, `0x70` = `0xabba`, `0x72` = `0x0007`. (`0xabba` looks like an unlock value for SigmaTel's extended registers; Hypothesis.)
3. Read extended audio ID `0x28` (`0x0080`) and status `0x2a` (`0x0080`); read powerdown status `0x26` (`0x000f`) and set it to `0x800f`.
4. Set mixer levels, then fade the master volume (`0x02`) from `0x3f3f` to `0x0808` in 56 steps of about 0.12 ms each.

Full per-register tables are produced by `vortexprobe.py codec` from any boot trace.

## A3D engine

Each positioned sound is a source holding a 56-tap FIR filter per ear, an interaural time delay (ITD), a gain and an "atmospheric" filter. The driver computes all of them on the host and writes them as **targets**; the chip keeps its own **current** values and glides them toward the targets, so parameter changes don't click. Direction lives in the FIR taps and the ITD; distance and occlusion live in the gain and atmospheric filter. The register names come from ALSA's `au88x0_a3d.h`; the behavior described here is what the traces and recordings show.

### Memory layout

There are 16 source slots: four 8 KB slices, each with four sources. This matches the `NR_A3D 0x10` constant in ALSA's `au8830.h`.

| Offset from source base (bank B) | ALSA name | Observed |
| --- | --- | --- |
| `+0x000`, 56 words | `HrtfCurrent` | Written when a sound starts; otherwise maintained by the chip |
| `+0x0e0` / `+0x0e4` | `GainCurrent` / `GainTarget` | Target rewritten during play; this is the distance and occlusion gain |
| `+0x0e8` / `+0x0ec` | `A12Current` / `A21Target` | Atmospheric filter feedback coefficients (two 16-bit values) |
| `+0x0f0` / `+0x0f4` | `B01Current` / `B10Target` | Atmospheric filter feedforward coefficients (two 16-bit values) |
| `+0x0f8` / `+0x0fc` | `B2Current` / `B2Target` | Always 0 in our traces |
| `+0x100`, 56 words | `HrtfTarget` | Rewritten continuously during play |
| `+0x1e0` / `+0x1e4` | `ITDCurrent` / `ITDTarget` | Target rewritten when the direction changes. Lower half = left-ear delay, upper half = right-ear delay; only the far ear is nonzero. `0x200` = one 48 kHz sample, up to about `0x5000` |
| `+0x1e8`, 56 words | `HrtfDelayLine` | Filter state; written at startup |

The source base is `0x19000 + slice × 0x2000 + source × 0x2c8` (ALSA `a3d_addrB`). Example: slice 3, source 2 is `0x1f590`, so its filter target is `0x1f690`. A second per-source bank, `0x18000 + slice × 0x2000 + source × 0x3a4` (`a3d_addrA`), holds the same names plus the ITD delay line (`+0x2c8`, the 45-word blocks written at startup), tracking time constants (`+0x37c–+0x388`) and filter state. Our traces show the driver using bank B during play. Each slice also has control registers at `0x19c00–0x19c40 plus slice × 0x2000` (`VDBDest`, `VDBSource`, `ABReg`, `CReg`, `Control`, `DebugReserved`, `Pointers`); `DebugReserved` (`0x19c3c` in each slice) is the one word the driver changed there between reset and idle.

Slot allocation (Confirmed, test-suite run 2): the driver hands out slots from 14 downward. The first 3D sound goes to slot 14 (slice 3, source 2), the next to 13, and so on down to 0, and slots are freed in reverse. With 16 sounds requested, only 15 slots were ever used; slot 15 was never given to a source.

At reset the implemented words are zero and the gaps between them read `0xDEADBABE` (see Address map).

### Filter coefficients

Each of the 56 words packs two signed 16-bit taps: **upper half = right ear, lower half = left ear** (Confirmed). With the recorded output lined up against the trace, the left/right level difference of the audio follows the coefficient balance (correlation −0.81); when the upper half carries more energy, the right channel is louder.

- The taps decay from the start of the block, the usual shape of a head-related impulse response.
- The louder (near) ear peaks at tap 0; the quieter (far) ear peaks 3 to 8 taps later. The main interaural delay is carried separately by `ITDTarget` (Confirmed): the lower half delays the left ear and the upper half the right ear, and `0x200` is one sample at 48 kHz (a 16-bit delay with 9 fractional bits). The driver's values follow a spherical-head (Woodworth) curve: 7 samples at 15°, 19 at 45°, 35 at 90°, and a maximum of 37–38 samples (about 0.78 ms) just behind the side. Elevation shrinks it (at 90° azimuth: 22 samples at 45° up, 0 straight up). Exactly 180° is a special case in the driver (see the direction table below). In the recording, the interaural lag matched the register to within about a sample wherever the click cross-correlation was clean (15°, 180°, 195°, 210° and 330°).
- As a sound circles the listener, the energy balance between the halves swings smoothly between about +15 dB and −16 dB.
- Total coefficient energy does not change with distance; distance is handled by the gain and atmospheric filter.
- Updates arrive roughly 8 to 20 times per second per source, depending on the program.

Tap scaling (Strong): the taps behave as Q15 at the same overall level as a plain (non-3D) source. Clicks recorded at 44 positions were compared with the clicks predicted from the register taps, ITD and gain. Once the speaker crosstalk below is accounted for, the recording matches the prediction to 4–10% residual below 2.4 kHz. The fitted overall scale of the 3D path (−3.7 to −9.2 dB across octave bands, average about −6.5 dB) agrees within about 2 dB with a native click through the same recording chain (−7.3 dB). The left and right tables are separate measurements, not mirror images: at 90° the near-ear taps correlate 0.87 with the mirrored 270° near-ear taps. The exactly-180° result is a separate case, described with the direction table.

### Parameter area

Earlier versions of these notes called the eight words before the filter target `p0`–`p7`; the analysis scripts still use those labels.

| Label | ALSA name | Observed values | Meaning |
| --- | --- | --- | --- |
| `p0` | `GainCurrent` | Written once when a sound starts, as a copy of the first target | Chip-maintained current gain (Strong) |
| `p1` | `GainTarget` | Both halves equal; `0x7fff` when nearest, down to about `0x1554` when far; drops sharply when a wall comes between | Distance and occlusion gain (Confirmed) |
| `p2` | `A12Current` | Written once at start | Current atmospheric coefficients |
| `p3` | `A21Target` | 0 when nearest; lower half grows with distance and jumps when occluded, upper half goes slightly negative | Atmospheric (muffling) filter, feedback side (Strong) |
| `p4` | `B01Current` | Written once at start | Current atmospheric coefficients |
| `p5` | `B10Target` | `0x4000` when nearest, falling with distance and occlusion | Atmospheric filter, feedforward side (Strong) |
| `p6`, `p7` | `B2Current`, `B2Target` | Always 0 | Third feedforward coefficient, unused here |

The current-versus-target behavior is visible on real hardware. In a running snapshot during A3D, the card read back gain `0x4183` while the driver's last target was `0x4234`, and the filter coefficients sat similarly between old and new targets. The glide is done by the chip (Confirmed): for an abrupt change the driver writes the new target once and never touches the current value. Measured from the recordings of both test-suite runs:

| Value | Time-constant word (ALSA name, driver value) | Glide at the driver's value | Confidence |
| --- | --- | --- | --- |
| Gain | `GainTrackTC`, `0x300` | First-order lag in linear amplitude, 45–70 ms (runs 1–4) | Confirmed |
| Atmospheric filter coefficients | `CoeffTrackTC`, `0x300` | Exponential glide of the coefficients themselves, 50–62 ms. This reproduces the lopsided response: switching the filter in takes about 400 ms to sound finished, switching it out about 100 ms | Confirmed |
| HRTF taps | `HrtfTrackTC`, `0x147` | About 115–130 ms | Confirmed |
| ITD | `ITDTrackTC`, `0xc8` | About 80–90 ms | Confirmed |

The four time-constant words sit in bank A at +0x37c (`HrtfTrackTC`), +0x380 (`GainTrackTC`), +0x384 (`CoeffTrackTC`) and +0x388 (`ITDTrackTC`). The driver writes `0x147`, `0x300`, `0x300` and `0xc8` when a source starts and 0 when it stops; these are also ALSA's defaults. Run 4 rewrote each word from the host while a source played (headphone mode, two 9-second cycles per value) and measured the glides from the recording (Confirmed):

| Word | Value | Glide measured | 2²¹ / TC samples |
| --- | --- | --- | --- |
| `GainTrackTC` | `0x100`, `0x180`, `0x300`, `0x600`, `0xc00` | 140–180, 93–124, 55–70, 25–41, about 25 ms | 171, 114, 57, 28, 14 ms |
| `CoeffTrackTC` | `0x100`, `0x180`, `0x300`, `0x600`, `0xc00` | 182, 116, 57–62, 33, 19 ms | 171, 114, 57, 28, 14 ms |
| `HrtfTrackTC` | `0x52`, `0x147`, `0x51e`, `0xc00` | 440, 124–129, 39, 21 ms | 533, 134, 33, 14 ms |
| `ITDTrackTC` | `0x32`, `0xc8`, `0x320`, `0xc00` | 220, 82–86, 35, under 21 ms | 874, 218, 55, 14 ms |

Each word controls only its own value: changing one left the other three glides unchanged. The audio measurements above were coarse; reading the current registers back directly settles the law (Confirmed, glidewatch session 2026-11-01, which polled the currents every 100 µs after each target change). The chip updates the currents once every **64 samples** (1.33 ms at 48 kHz), not every sample. Each update moves the value **TC × 64 / 2²¹** of the remaining distance for gain, filter coefficients and HRTF taps, and **TC × 64 / 2¹⁹** for the ITD. That makes the time constants 2²¹ / TC samples (gain, filter, HRTF) and 2¹⁹ / TC samples (ITD); the ITD is four times faster than the others at the same TC. Fitted τ × TC came out at 2.00–2.07 million for gain, filter and HRTF (2²¹ ≈ 2.10 million) and 0.49–0.52 million for the ITD (2¹⁹ ≈ 0.52 million), and at gain TC `0xc00` each update leaves exactly 0.90625 of the remaining distance (1 − 3072 × 64 / 2²¹). At the driver's defaults the glides are about 130 ms for the HRTF (`0x147`), 57 ms for gain and filter (`0x300`) and 55 ms for the ITD (`0xc8`). An emulator should step the values every 64 samples by these fractions.

**Occlusion** (A3D Rooms demo) appears as simultaneous jumps in all three targets. Example, one source:

| Trace time | Event | `GainTarget` | `A21Target` lower half | `B10Target` |
| --- | --- | --- | --- | --- |
| 57.2 → 57.3 s | Comes into view | `0x062c` → `0x1295` | `0x6a52` → `0x48b6` | `0x01d5` → `0x0bf0` |
| 69.0 → 69.2 s | Wall comes between | `0x0e5f` → `0x0b68` | `0x2f5b` → `0x4400` | `0x1967` → `0x0e0f` |
| 99.3 → 100.5 s | Comes back into view | `0x0163` → `0x036c` | `0x69f6` → `0x3a19` | `0x01e5` → `0x1316` |

Distance changes move the same three values smoothly; occlusion moves them in steps.

**The atmospheric filter is a two-pole low-pass with unity DC gain, in Q14 fixed point** (Confirmed). Reading the words as signed 16-bit values divided by 16,384:

- `A21Target` upper half = a2, lower half = a1; `B10Target` lower half = b0, upper half = b1 (always 0 so far); `B2Target` = b2 (always 0).
- The filter is y\[n\] = b0·x\[n\] + b1·x\[n−1\] + b2·x\[n−2\] + a1·y\[n−1\] + a2·y\[n−2\].
- Check: (b0 + b1 + b2) / (1 − a1 − a2) = 1.000 at the median of all 1,823 updates in the Rooms, distance and Helicopter traces (5th–95th percentile 0.994–1.000), and every pole lies inside the unit circle.
- Nearest setting: a1 = a2 = 0, b0 = `0x4000` = 1.0, a straight pass-through. Farthest helicopter setting: a1 = 0.896, a2 = −0.200, b0 = 0.305. Occluded Rooms source: a1 = 1.806, a2 = −0.815, b0 = 0.0095, a much darker double pole near 0.90.

So distance and occlusion both work by sliding this low-pass's cutoff down while the gain target drops. `GainTarget` is Q15 (`0x7fff` = 1.0), and the filter is always a double real pole; both are confirmed by the test suite below.

### Driver conversions measured with the test suite (Confirmed)

The 2026-10-31 a3dtest run changed one A3D API setting at a time (one source, 2 s per step) and logged each step with a timestamp, aligned to the trace within about 20 ms. Results for the active source (slice 3, source 2):

| API setting | Register result |
| --- | --- |
| `SetGain(g)` | `GainTarget` = g × `0x7fff` in both halves: 1.0 → `0x7fff`, 0.5 → `0x3fff`, 0.0625 → `0x07ff`, 0 → 0 |
| Distance r (min 1 m, max 100 m) | Gain = 1/r beyond the minimum: 2 m → `0x3fff` … 64 m → `0x01ff`; unchanged inside 1 m. HRTF taps and ITD do not change with distance |
| Distance, air absorption | Atmospheric low-pass with a double real pole p: a1 = 2p, a2 = −p², b0 = 1 − a1 − a2, b1 = b2 = 0. p = 0 (bypass) up to 1 m, then 0.175 at 2 m, 0.352 at 4 m, 0.524 at 8 m, 0.674 at 16 m, 0.804 at 32 m and 0.888 at 64 m; −3 dB points 10.9, 5.6, 3.3, 2.0, 1.1 and 0.6 kHz |
| `SetEq(α)` | Same filter: 1.0 bypass; 0.9 → 5.6 kHz; 0.75 → 2.9 kHz; 0.5 → 1.6 kHz; 0.3 → 1.1 kHz; 0.1 → 660 Hz; 0 → b0 = 0 (silent) |
| Occlusion (wall between, source 4 m away) | Low-frequency transmittance multiplies the gain; high-frequency transmittance alone sets the filter (0.5 → 1.3 kHz, 0.25 → 840 Hz, 0.1 → 550 Hz). Transmittance 1.0/1.0 changes nothing; 0/0 gives gain 0 and b0 = 0 |
| Azimuth, elevation | New HRTF taps and ITD only; gain and filter untouched. Straight up gives the same taps at any azimuth. Left and right tables are not exact mirror images |
| `SetPitch`, Doppler | No A3D register changes; `CONVRATIO` only (see Sample-rate conversion) |

The double-pole form also explains the earlier unity-DC check: with b0 = 1 − a1 − a2 the DC gain is exactly 1. The earlier demo traces fit it too (helicopter far: p = 0.448; occluded Rooms source: p = 0.903). An emulator needs only the biquad; a wrapper that replaces the driver needs this table.

### The driver's direction table (Confirmed)

Test-suite run 2 stepped one source through every 5° of azimuth at elevations −45° to +90° in 15° steps (649 positions, plus 1° steps from 170° to 190°) and recorded the taps and ITD the driver wrote for each. All 24 positions also measured in run 1 match exactly.

- The driver's table has an entry every 15° of azimuth. Positions in between are a straight linear interpolation of the two neighbouring entries, taps and ITD alike (second differences are 1–3 units, rounding).
- That gives 217 grid entries over the measured range (24 azimuths × 9 elevations from −45° to +75°, plus straight up). Each entry is 56 left and 56 right taps and a two-ear ITD.
- The ITD is fractional: 5° at −45° elevation gives `0x354` (1.66 samples), exactly a third of the 15° entry.
- **Exactly 180° is a special case, not the table's 180° entry (Confirmed).** A source placed exactly behind gets taps 3–9 times the amplitude of its neighbours and a large one-ear ITD (40 samples on the left ear from −15° to +30°, 16 at ±30–45°, 0 at −45° and above 45°), the same in every run. But it is not what the driver interpolates with: 179° and 181° (1° sweep at 0°), and 175° and 185° at every elevation, are linear blends of 165°/195° with a normal-looking 180° entry that has zero ITD and ordinary energy. The 180° entry reconstructed from the 165–180 side and from the 180–195 side agrees to 0.1% at every elevation. So the table's real 180° entry is ordinary, and an exactly-180° direction takes a different path in the driver (Hypothesis: the ±180° boundary of the angle calculation sending the lookup outside the table). Games would only hit it with a source exactly behind the listener. An emulator gets this from the driver either way; a renderer using the table should use the reconstructed entry.

Run 3-2 stepped elevation in 5° steps at four azimuths: the elevation grid is also 15°, with the same linear interpolation. Run 12 moved a source over a 3.75° grid in three regions (351 positions) and confirmed that the driver interpolates bilinearly in azimuth and elevation together: every position matched bilinear interpolation of the table to within rounding (0.05%), against 7–16% for nearest-point or one-axis schemes. [Appendix A](appendix-a-direction-table.md) describes the table and how to read it out of the driver with a card; its contents are not published here. A driver-replacing wrapper needs it; an emulator running Aureal's driver gets it from the driver.

Recorded audio during a helicopter WAV moving in distance (471 updates, 56 s) supports these readings:

| `p1` (fraction of `0x7fff`) | Updates | Relative level | Treble vs. midrange |
| --- | --- | --- | --- |
| 0.0–0.3 (far) | 187 | 27.3 dB | −26.7 dB |
| 0.3–0.5 | 133 | 31.2 dB | −24.7 dB |
| 0.5–0.8 | 68 | 35.0 dB | −23.4 dB |
| 0.8–1.0 (near) | 83 | 38.4 dB | −22.2 dB |

Level tracks `p1` (correlation 0.82 with 20·log10 of `p1`). Distant sounds are duller: the ratio of 4–12 kHz to 200 Hz–2 kHz energy falls about 4.5 dB from near to far, and tracks `p3` (correlation −0.59), as expected for a recorded rotor sound that varies on its own.

### Reflections, stereo mode and speaker output

**Reflections do not use extra A3D sources.** The Helicopter demo used a single source with Reflections on or off. Reflections instead show up in wavetable voice 0's parameter registers. This fits a developer's account on VOGONS that A3D 2.0 WaveTracing is computed in `A3DAPI.dll` and the results are sent to the wavetable engine.

| Register (ALSA WT name) | Reflections on | Reflections off |
| --- | --- | --- |
| `0x200` / `0x204` (`PARM0` / `PARM1`), upper half | `0xcca6–0xccb2`, rising and falling with the helicopter's orbit (about 10 s per cycle) | Fixed `0x8400` |
| `0x20c` (`PARM3`) | Varies, `0xcfefd8ff–0xcfefffff` | Fixed `0xcfefffff` |

The reflection delay setting has not been located. The WT delay registers (`0x400–0x40c`) stay 0. Moving the slider produced step changes only in FIFO control (`0x16000`, `0x16100`) and the DMA start registers (`0x27b00`, `0x27c00`), which looks like the demo restarting its sound buffers; that fits the reflection delay being applied by timing rather than by a register (see below). One run with the slider at an extreme left reflections in their off state throughout. In the test suite (a reflective wall 2 m to the side), changing the delay scale or the maximum delay time changed no register value either. The gain scale did: bits 16–23 of `PARM0`/`PARM1` went `0xa0` → `0x90` for ×0.5 and `0xb0` for ×2, so that byte is a logarithmic gain with `0x10` per 6 dB (Strong). Setting the wall's reflectance to 0.5 (low) / 0.1 (high) gave `0xa090a000` instead of `0xc6a0a000`: the gain byte dropped 6 dB and the top byte changed from `0xc6` to `0xa0`, so the top byte probably carries the high-frequency (filter) part (Hypothesis).

The recording settles the delay question (Confirmed). In the test suite the source was 4 m ahead with a wall 2 m to the right, so the reflected path is 1.66 m longer, or 4.8 ms at 343 m/s. An echo appears about 4 ms after each click when the wall is present and not without it. It moves to about 2, 9 and 19 ms for delay scales 0.5, 2 and 4, and its level changes by about −7 and +6 dB for gain scales 0.5 and 2. No chip register changed with the delay scale, while the wavetable DMA page pointers (`0x27000–0x2700c`, with `0x27600`/`0x27604`) cycle continuously. The reflection signal is therefore computed on the host, delay included, and streamed to the card through a wavetable DMA channel; the chip only plays it at the gain set in `PARM0`/`PARM1`.

Run 2 mapped the reflection voice's `PARM0`/`PARM1` fields (Strong). The two words differ only in the lowest bit (`0x…a000` and `0x…a001`), which looks like a left/right voice pair.

| Change (wall 2 m right unless noted) | `PARM0` | Echo in recording |
| --- | --- | --- |
| Reflectance 1.0 / 1.0 | `0xc6a0a000` | 4 ms, −10 dB |
| Low reflectance 0.5, 0.25 | `0xc690…`, `0xc680…` | −14, −21 dB |
| High reflectance 0.5, 0.25, 0.1 | `0xb0a0…`, `0xa7a0…`, `0xa0a0…` | −16, −17, −21 dB (duller) |
| Wall at 1 m, 4 m, 8 m | `0xc8a6…`, `0xc293…`, `0xba83…` | 4 m: about 14 ms (predicted 14.4) |
| Wall behind, 2 m | `0xc19ca000` | 11 ms (predicted 11.7) |
| Wall on the left, 2 m | `0xc6a0a05e` (`PARM3` also changed) | Irregular; possibly the test's polygon winding |

Bits 16–23 are the reflection gain in steps of 6.02/16 = 0.376 dB, and they include the longer path. Bits 24–31 follow the high-frequency reflectance and the distance, and they set a low-pass filter (see run 14 below). With two or three walls the driver still used only this one voice pair, so additional reflections are mixed on the host into the same stream.

**Test-suite run 14 (`run13`, wall 4 m to the right, echo 13–15 ms after each click) measured both bytes against the recorded echo (Confirmed).**

- **Gain byte.** Lowering the wall's low-frequency reflectance dropped the gain byte from `0x93` to `0x8c`, `0x83`, `0x7a`, `0x73` and `0x5e` for reflectance 0.75, 0.5, 0.35, 0.25 and 0.1. The echo fell by the same amount at every frequency: 3.6, 7.0, 9.2, 12.5 and about 19 dB. So low-frequency reflectance is pure gain, at `0x10` per 6 dB, and reaches the chip only through this byte.
- **Top byte.** Lowering the high-frequency reflectance changed only the top byte: 1.0, 0.75, 0.5, 0.35, 0.25, 0.15, 0.1, 0.05 and 0 gave `0xc2`, `0xb6`, `0xac`, `0xa7`, `0xa4`, `0xa1`, `0x9d`, `0x95` and `0x84`. Dividing each step's echo spectrum by the full-reflectance step's (1/3-octave bands, 300 Hz to 10 kHz, only where at least 6 dB above the noise) cancels the head-related and recording responses. What is left fits a two-pole low-pass with Q ≈ 0.64 (0.7 dB rms), better than one, two or three cascaded one-pole sections (1.8, 1.0 and 0.8 dB, with worse residual shapes). The fitted cut-offs were 4.7 kHz, 2.1 kHz, 1.2 kHz, 920, 800, 640, 570, 455 and 250 Hz. They follow log2(fc) = 13.505 + 1.817u + 0.139u², with u = (byte − `0xcf`) / 16, to within 0.8 dB rms. That law puts `0xcf` at about 11.6 kHz. Since the driver uses `0xcf` for voices that must pass unchanged, `0xcf` and above are taken as no filter (Strong).
- **Echo level.** Below about 500 Hz, where neither the filter nor the crosstalk canceller (next point) does much, the echo's level relative to the direct click matched the emulator within about 1 dB. So the reflection gain needs no correction.
- **Crosstalk canceller.** The X79 machine's driver stayed in satellite-speaker mode throughout runs 2, 13 and 14 (`0x24660` = `0xc085ad89`, delay 13), even though A3DTEST asked for headphones and the call succeeded. So on the real card, the direct (A3D) sound in these recordings went through the canceller, which adds about 8 dB around 1–1.5 kHz, while the echo (wavetable voice → mixer) did not. Comparisons of echo against direct sound with the emulator need the same speaker setting on both.

**Wall materials in the A3D Player demo (Confirmed from its code).** Player's Wood, Concrete, Steel and Carpet choices for the inner and outer walls are just preset numbers passed to `IA3dMaterial::SetReflectance` and `SetTransmittance` (its code also calls `SelectPreset` when a preset index is given, but every table entry uses −1, and the SDK documents `SelectPreset` as unsupported):

| Material | Reflectance low / high | Transmittance low / high |
| --- | --- | --- |
| Wood | 0.5 / 1.0 | 0.5 / 0.5 |
| Concrete | 0.75 / 1.0 | 0.25 / 0.25 |
| Steel | 1.0 / 1.0 | 0 / 0 |
| Carpet | 0.25 / 1.0 | 0.75 / 0.75 |

The table's order follows the dialog list as stored (Wood, Concrete, Steel, Carpet); if the list were sorted, Wood and Carpet would swap. The default before any choice is reflectance 0.85 / 0.85, transmittance 0.3 / 0.3. These drive the occlusion and reflection mappings already documented above, so materials involve no further hardware.

**Stereo mode** (Helicopter's "Audio: Stereo") leaves the A3D sources idle. Mixer input volumes ramp continuously instead: current values at `0x202xx`, targets at `0x212xx`. That is ordinary panning through the mixer, again with current/target pairs.

Plain (non-3D) playback is a clean unity path (Confirmed, run 2): a 48 kHz click in only the left or only the right channel comes out of that channel at the same level as the recording chain's own gain (−7.2 dB, flat from 150 Hz to 10 kHz), with the other channel more than 45 dB down. A mono or stereo-both click reaches the left output after the right, by 8–9 samples in run 1 and about 15 samples in run 2. The 3D path shows no such offset. Whether this comes from the native routing or the codec is still open.

**Speaker crosstalk cancellation** lives at `0x24000–0x25fff` (ALSA au88x0\_xtalk.c), the block that changes only once A3D starts. Run 1 of the test suite (default output mode, which is a speaker mode) shows it working (Confirmed). Each output channel equals that ear's FIR result plus X(f) times the other ear's, with the same X for both channels. Fitted over 44 click positions, |X| is about 1.0 at 150–300 Hz, 0.9 at 300–600 Hz, 0.64 at 0.6–1.2 kHz, 0.46 at 1.2–2.4 kHz and about 0.25 above that. Its phase is close to an inversion delayed by about 0.4 ms. In other words, it is a classic canceller: the far channel gets an inverted, delayed, low-passed copy of the near channel. Adding it cuts the model error from 40–80% to 4–10% below 2.4 kHz. It explains why the recorded left/right swing is much smaller than the filter balance. The A3D API's `SetOutputMode` does not change it (Confirmed, run 2). Headphones, wide speakers and narrow speakers gave the same X to two decimal places, and no register in `0x24000–0x25fff` changed; narrow speakers returned E\_INVALIDARG. The mode is presumably taken from Aureal's control panel, so measuring the canceller off, and its coefficients per mode, needs the panel setting changed between runs.

**Speaker types (Confirmed, runs 3–5).** The control panel's speaker setting loads one of three coefficient sets into the canceller. All three are word for word the tables in ALSA's au88x0\_xtalk.c:

| Control panel setting | ALSA table | Canceller delay word | Measured crosstalk X (225 Hz / 900 Hz / 3.6 kHz) |
| --- | --- | --- | --- |
| Satellite speakers (the default) | `Wide` | `0xd` | 1.04 / 0.63 / 0.31, inverted, about 0.45 ms |
| Monitors | `Narrow` | `0x7` | 1.02 / 0.75 / 0.53, inverted, about 0.25 ms |
| Headphones | `Pipe` (pass-through: `0x0fa0` in each section, zero crosstalk filter) | — | 0.01 / 0.01 / 0.07 (none) |

ALSA's code gives the register layout: for each ear an EQ filter and a crosstalk (XT) filter of five 5-word sections each (`0x24200`, `0x242b4`, `0x24368`, `0x2441c`, each + section × 0x24), gains and shifts at `0x24538–0x24554`, and the delay at `0x24660`. It does not give the arithmetic.

**Structure (Confirmed, runs 9 and 10, host writes).** Each side has an EQ stage, and the crosstalk is mixed from the other side's EQ output. Zeroing both EQ gain words changes the left and right outputs by the same complex factor, and raising the left EQ's shift raises both outputs. ALSA's "XT left" filter feeds the right output and "XT right" the left. With every gain word at 0 the canceller passes sound straight through, which is what headphone mode does; the filter sections are then irrelevant.

**Arithmetic (Open).** Two host-side sweeps did not decode it. Run 10 used synthetic coefficients: a pure gain, a pure delay or a gentle one-pole term in one section, and each gain and shift word on its own. The added path kept the same shape whatever the section words held. Pure delays (b1 or b2 only) and small feedback terms changed it by under 5%, and it grew far less than proportionally with the gain word. Reading the five words as any ordering of an ordinary biquad, at any scale from Q12 to Q15, misses the measurements by about 80%. So the sections are not a plain biquad cascade, or they are not latched when written from outside; the driver may need a separate update step.

**Working model (Strong).** Since only Aureal's three presets are ever loaded, the canceller can be emulated by its measured response instead. Per ear, output = E ∗ (own input + X ∗ other input). E and X are 512-tap FIRs at 48 kHz fitted for satellites (Wide) and monitors (Narrow); headphones are a pass-through. Fitted on runs 2 and 5 and tested on run 1, an independent session with 44 click positions, the model's error is 4.5–10% below 2.4 kHz and 19–28% above (the recording's own limit there), against 33–80% without crosstalk. The filters, a C header and a Python reference implementation are in `data/xtalk/` ([Appendix B](appendix-b-crosstalk-model.md)). The model adds 128 samples of latency, which an emulator can trim; the real chip's latency is not measured.

**Four-speaker output (Confirmed, runs 6 and 7).** The rear jack is always live. The route list sends mixer outputs 0/1 to codec slots 0/1 (front) and mixer outputs 2/3 to codec slots 4/5 (rear) in both stereo and quad mode. What changes is the mixer:

- **Stereo:** the rear mix is a copy of the front mix (the 2D sound and the crosstalk canceller's output, at the same gains).
- **Quad:** the 2D mix goes to all four outputs at −3.8 dB. The A3D/canceller output goes to the front only. The rear instead gets a plain copy of each 3D source, without HRTF processing, through its own mixer input (input 26 in this run, fed by a wavetable channel), panned left/right by the driver. The driver also turns the source's front `GainTarget` down as it moves behind the listener.

| Azimuth | Front 3D gain | Rear left | Rear right |
| --- | --- | --- | --- |
| 0°, 30° | 0 dB | off | off |
| 60° | −0.2 dB | off | −19.5 dB |
| 90° | −1.8 dB | off | −10.5 dB |
| 135° | −10.2 dB | off | −6.0 dB |
| 180° | off | −3.8 dB | −7.5 dB |
| 270° | −1.8 dB | −10.5 dB | off |

So quad mode is a front/rear crossfade done by the driver with ordinary mixer gains; the chip needs no extra 3D hardware for it. In quad mode the driver gave out only 11 hardware 3D slots (14 down to 4) instead of 15. In run 6 the speaker type was satellites; the rear did not get its own canceller.

**The EQ block (Confirmed, run 9, host writes).** The chip's EQ at `0x2b000` sits in the 2D path: mixer outputs 6/7 → EQ → mixer inputs 7/8 → front and rear outputs. The Windows driver leaves it transparent: every band gain is 0 and the bypass gain (`0x2b3d4` left, `0x2b3ec` right) is `0x7fff`. Switching it on from the host showed:

- Output = bypass gain × input + the sum of ten band-pass filters, each times its band gain (`0x2b02c` + band × 0x30 left, `0x2b20c` + band × 0x30 right). Bypass 0 with every band at 0 silences the 2D mix (about 50 dB down). Bypass `0x3fff` gives −6 dB, so the bypass gain is linear Q15.
- The bands are octaves centred on 31, 63, 125, 250, 500 Hz, 1, 2, 4, 8 and 16 kHz. Each is a band-pass about 6 dB per octave down on either side of its peak.
- ALSA's "normal" band gain `0x3e96` gives unity at the band centre, and all ten at `0x3e96` with bypass 0 sum to flat within about 1 dB.

For an emulator driven by the Windows driver, a pass-through is exact; the band model above covers anything that programs the EQ itself.

### What the driver receives from the A3D library

Aureal's A3D library does not send filters to the driver. According to the re-a3d-api reconstruction, it sends one 1,036-byte control record per sound (`A3DCTRL_SRC_SUPER`) through a DirectSound property set, and Aureal's driver turns that record into the register values documented above. The record holds:

| Field | Contents | Likely register result |
| --- | --- | --- |
| Per ear: `fAzim`, `fElev` | Direction of the sound for that ear | HRTF filter taps (`HrtfTarget`), looked up by the driver |
| Per ear: `fGain` | Gain | `GainTarget` |
| Per ear: `fDelay` | Delay | `ITDTarget` |
| `fAlpha` | "Equalisation" | Atmospheric low-pass coefficients |
| `fPitch`, `fFreqFactor` | Pitch and frequency factor | Sample-rate converter `CONVRATIO` (Doppler) |
| `fMinDist`, `fMaxDist`, `fDistScale`, `fDistance` | Distance model | Folded into gain and `fAlpha` (Hypothesis) |
| `Reflections[16]` | Per reflection: enable and mute flags, `fAlpha`, per-ear direction, gain and delay | Reflection rendering (wavetable voices) |

The right-hand column is our inference from the traces; the field list is from the reconstruction. Two consequences:

- **The HRTF tables live in Aureal's driver.** The library's own software renderer uses at most 50 taps, while the chip receives 56, so the driver has separate tables. An emulator that runs the original driver gets them for free; a wrapper that replaces the driver needs them.
- **Reflection delay is a time per reflection, not a register.** On the DirectSound route the library builds reflections by duplicating the sound buffer and starting the copy later, which matches the buffer restarts seen when the delay slider moved.

Logging these records during a traced session would give the driver's input and output side by side, and with them the exact conversions from gain, delay and `fAlpha` to register values.

## Routing, sample rates and timing

The Windows driver's audio graph, sample-rate arithmetic and master clock can now be read straight from snapshots and traces (`vortexprobe.py decode`).

### Routing (Confirmed)

The ADB route table is a set of linked lists. `CHNBASE` (`0x282b4` + channel × 4) holds each ADB channel's first route; a route is `source << 8 | sink`; `RTBASE` (`0x28000` + (route & 0xff) × 4) holds the next route; `0xffff` ends the list. This matches ALSA's `vortex_adb_addroutes`. Decoded from the idle, WAV and A3D snapshots:

| Function | Routes | Present |
| --- | --- | --- |
| Front output | `MIXOUT0/1` → `CODECOUT0/1` | Always |
| Rear output (4-channel codec) | `MIXOUT2/3` → codec sinks `0x8c/0x8d` | Always |
| S/PDIF | `MIXOUT8/9` → `SPDIFOUT0/1` (ADB channel 20) | Always |
| Hardware EQ loop | `MIXOUT6/7` → `EQIN0/1`; `EQOUT0/1` → `MIXIN7/8` | Always |
| Mixer feedback | `MIXOUT4/5` → `MIXIN1/2` | Always |
| Wavetable | `WT0_32/33`, `WT1_32/33` → `MIXIN3–6` | Always |
| Input monitor | `CODECIN0/1` → `MIXIN10/9` | Always |
| WAV playback | `ADBDMA0` → `SRCIN0/1` → `SRCOUT0/1` → mixer | While a sound plays |
| A3D sources | `ADBDMA2/3` → `SRCIN4/5` → `SRCOUT4/5` → `A3DIN15/14` | A3D demo |
| A3D output | `A3DOUT0–7` → `XTALKIN0, 5, 1, 6, 2, 7, 3, 8`; `XTALKOUT0/1` → `MIXIN11/12` | A3D demo |
| Extra wavetable voices (reflections) | `WT0_36/37`, `WT1_36/37` → `MIXIN14–17` | A3D demo |
| Second stereo stream | `ADBDMA1` → `SRCIN2/3` → `SRCOUT2/3` → `MIXIN0/13` | A3D demo |

So every A3D source passes through a sample-rate converter, the A3D engine and then the crosstalk canceller before the mixer. The always-present routes were also in the "cold" read, which means a warm reboot does not clear the route table: memory-like regions in that read hold leftovers from the previous Windows session, not true power-on contents.

### Sample-rate conversion (Confirmed)

`CONVRATIO` (`0x26e40` + converter × 4) is the input rate divided by 48 kHz, in unsigned fixed point with 14 fractional bits (`0x4000` = 1.0).

| `CONVRATIO` | Input rate |
| --- | --- |
| `0x3acd` | 44,100 Hz (the demos' WAV files) |
| `0x1d66` | 22,050 Hz |
| `0x1000` | 12,000 Hz |
| `0x0eb3` | 11,025 Hz |
| `0x0aab` | 8,000 Hz |

Doppler is applied here: during A3D play the moving source's converter is rewritten about 60 times per second with values from `0x3ac8` to `0x3ad1` (44,086–44,112 Hz). The test suite confirmed the formulas, with the A3D source on converter `0x26e5c`: `SetPitch(p)` settles at p × `0x4000` (0.5 → `0x2000`, 2.0 → `0x8000`), and a velocity v along the line of sight settles at c/(c + v) × `0x4000` with c = 343 m/s (+40 m/s → `0x3946`, −40 m/s → `0x4885`). The ramp is done by the driver, not the chip: it walks the ratio to the new value over about 8–9 writes (a few hundred milliseconds) instead of writing it once. The other converter words (`U0–U3`, drift) are not decoded; `U0`'s low byte is `0xc0` plus the converter number when in use. Unused converters and DMA channels hold random power-up contents. Non-3D playback at 8, 11.025, 22.05, 44.1 and 48 kHz gave `0x0aab`, `0x0eb3`, `0x1d66`, `0x3acd` and `0x4000`; 8-bit and stereo buffers changed no converter or DMA configuration word we could identify.

**Converter filter (Confirmed, run 12).** DirectSound buffers of noise, sweeps and single-sample impulses at 8, 11.025, 22.05, 32, 44.1 and 48 kHz were recorded and each compared with 48 kHz, which cancels the recording chain. Relative to the input rate the response is the same at every rate: flat to 0.45 × the input rate, −3 dB at 0.48, −6 dB at 0.50, −11 dB at 0.52, −23 dB at 0.55, −37 dB at 0.57, and below the recording's noise floor (about −60 dB) from 0.60. So the converter is a proper band-limited interpolator with its cutoff at the input's Nyquist frequency, and low-rate sounds come out with no audible images above it. 8-bit input and an arbitrary playback rate (22.05 kHz data played at 30 kHz) behave the same way. A 22-tap Kaiser-windowed sinc (β 6) matches the measurements within their scatter; the 86Box model now uses it in place of 4-point interpolation. Downward conversion (input rates above 48 kHz) was not tested.

**Levels and headroom (Confirmed, run 12).** Output level is linear (full, half and one-tenth scale tones measured 1 : 0.499 : 0.100), the converter has unity gain (a half-scale tone at 22.05 kHz matched 48 kHz within 0.04 dB), and an A3D native source and a 3D source straight ahead at the minimum distance in headphone mode both play at the source's own level. Summing half-scale tones of different pitches showed that the output saturates at digital full scale: two tones (peaks reaching full scale) stayed clean, three left distortion products 29 dB below the signal with the peaks held at full scale, and four 15 dB below. The output does not wrap around.

### DirectSound buffers and the mixer (Confirmed, run 2)

Ordinary DirectSound hardware buffers, the path most non-A3D games use, map onto the chip like this:

- **Volume and pan** go to mixer input gains: targets at `0x212xx`, current values at `0x202xx`, one word per input per output side (for example `0x21238` left and `0x212b8` right). The word is a signed 8-bit gain with `0x08` = unity (measured 0.0 dB against the plain playback path) and about 0.75 dB per step: −600 mB → `0x00` (−6.0 dB), −1200 → `0xf8`, −2400 → `0xe8`, −4800 → `0xc8` (−48.2 dB), −10000 → `0x80` (silent). Pan attenuates only the opposite side, with the same law. The chip glides current toward target, as in the A3D engine.
- **Frequency** goes to the buffer's converter as frequency / 48 kHz × `0x4000`, ramped by the driver over about 10 writes (24 kHz → `0x2000`, 96 kHz → `0x8000`).
- **Sample format** is in the ADB DMA control word (`0x27a00` + channel × 4): `0x22000` for 16-bit and `0x6000` for 8-bit. Read with ALSA's field layout, bits 14–17 hold 8 (signed 16-bit) or 1 (unsigned 8-bit). Mono versus stereo is not in that word; stereo buffers get a second route.
- 8 kHz buffers were refused in hardware (DSERR\_ALLOCATED) and fell back to software mixing; 11.025–48 kHz were all accepted.

**Sound Blaster in a DOS window (Confirmed, Duke Nukem 3D setup).** Sound Blaster emulation in a Windows 98 DOS window is done in software by Aureal's driver, which plays the game's audio through the ordinary playback path: an ADB DMA channel → sample-rate converter → mixer, the same route DirectSound uses. The converter is set to exact Sound Blaster time-constant rates: 10,989 Hz (time constant 165), 21,739 Hz (210), 10,752 Hz and 6,000 Hz. The DMA control word is `0x7000`, which is unsigned 8-bit with the DMA interrupt enabled; ordinary Windows playback leaves it off. So the driver apparently points the card's DMA at the DOS program's own 8-bit buffer and uses the DMA interrupt to raise the emulated Sound Blaster interrupt. No legacy I/O-port decoding by the card is involved, so an emulator running Aureal's Windows driver gets DOS-window Sound Blaster support with no extra hardware model. BAR2 was touched only at boot (one write/read/write at offsets 4 and 6). Selecting General MIDI crashed the setup program. Real-mode DOS (Restart in MS-DOS mode) is untested.

**Sound Blaster test program (Confirmed, sbpro session 2026-11-01).** A small test program (DSP reset at 220h, version query, one second of single-cycle 8-bit DMA at 11 kHz on DMA 1, mixer master volume) passed three times in a Windows 98 DOS window: DSP version 3.02, IRQ 5 delivered, DMA count exhausted, and the tone audible in the recording. During playback the driver touched no register outside the ordinary wave path: ADB DMA channel 0 cycled through four 1 KB pages, with the page-done interrupt raising the emulated IRQ. A replay of the whole session through the 86Box model matched every register read the driver made in that window.

**Boot-time Sound Blaster set-up (Strong).** When Sound Blaster emulation is enabled, something runs at boot, before the Windows driver loads, and talks to the card through the I/O window (BAR1). It sets bit `0x01000000` in `CHIP_CTRL` (`0x2a00c`), plays ADB DMA channel 1 from four 256-byte pages in low memory (`0x6500`–`0x68ff`) through a sample-rate converter to the mixer, briefly enables the timer interrupt, then masks every interrupt and leaves `CHIP_CTRL` at `0x01103260`. The Windows driver later reads the top byte of `CHIP_CTRL` (1 when emulation is on) and writes `0x212eb` to `0x2a804`. The meaning of bit `0x01000000` and of `0x2a804` is unknown; they may enable the chip's own legacy port decoding for real-mode DOS. Real-mode DOS (outside Windows) cannot be tested under QEMU passthrough, because QEMU does not forward legacy ISA ports such as 220h to the card; it would need bare metal.

**Resource conflict that freezes the machine (Confirmed in an 86Box VM).** The emulation reserves fixed legacy resources: I/O 220–22Fh, IRQ 5 and DMA 1. Device Manager offers only one basic configuration, and Windows will not let the IRQ be changed even with automatic settings turned off. If Windows' ACPI IRQ steering has put a PCI device on IRQ 5, the two collide. In an 86Box ASUS P2B-LS machine the Intel 82371AB/EB USB controller sat on IRQ 5 (Device Manager listed it, with the ACPI IRQ holder, as the conflict), and with Sound Blaster emulation enabled Windows froze at boot just after the startup sound; disabling the emulation fixed it at once. Because the emulation is Aureal's own driver code, a real SQ2500 in the same board should behave the same way. The fix is to disable Sound Blaster emulation in Device Manager, or to move the other device off IRQ 5 (disable it, or reserve IRQ 5 for legacy ISA in the BIOS where that is offered).

**Recommendation for emulator users.** The Vortex's Sound Blaster support is a software layer for DOS windows, compatible only to Sound Blaster Pro (DSP 3.02), and it costs a fixed IRQ that can collide with PCI devices. For DOS games, an emulated Sound Blaster 16 or similar ISA card is the better choice; leave the Vortex's Sound Blaster emulation disabled unless something specifically needs it.

**MIDI is software synthesis (Confirmed, run 12).** The driver offers two MIDI devices, "Vortex AU8830 MPU-401" and "Vortex AU8830 Wavetable" (576 voices). The wavetable device does not use the chip's wavetable voices: during a full MIDI test (notes across the keyboard, velocities, eight instruments, drums, controllers and 40 s of CANYON.MID through the MIDI mapper) no voice parameters or wavetable DMA were written. The driver renders the music on the host and streams it as PCM through ADB DMA channel 0, driven by a fast sample timer (periods of about 0.3–1.7 ms, roughly 500 interrupts a second). It also polls word `+0x10` of each wavetable bank (all ones on the real card) and writes `0x00880000` to `+0x04` about 100 times a second. An emulator that plays DMA streams and services the timer therefore reproduces the MIDI output exactly, with no instrument model.

### Wavetable voices in a game (Half-Life, A3D mode)

Five minutes of Half-Life play (session `20261031-203721-hl-play2`, interrupts alive throughout) used the wavetable engine far more than any earlier test. About 8,700 sounds started in five minutes, each on its own voice in the second half of the engine (voices 4–29 at `0x8200` + voice × 0x10, with wavetable DMA channel 32 + voice). Up to 28 voices were active at once, alongside about 12 of the 16 A3D sources. Voice starts did not coincide with A3D source starts or with ADB DMA starts. DirectSound in our test suite used ADB channels and converters instead, so this is a separate path.

Starting a voice: the driver clears the voice's 64-word FIFO (`0x10000` + global voice × 0x100) and its FIFO control word (`0x16000` + global voice × 4), and sets up the wavetable DMA ring (`0x27000`/`0x27600`/`0x27900` + channel). It then reads and clears `0x8008` and `0x8080` + (voice ÷ 2) × 4, and writes PARM0–3. While the voice plays, the driver writes 0 to `0x8180` + voice × 4 at each DMA refill. The roles of those three words are unknown. The DMA control word is `0x22000`, which means signed 16-bit.

**PARM0/PARM1 layout (Strong).** In steady state PARM0 and PARM1 are the same apart from bit 0 (0 in PARM0, 1 in PARM1), so each voice gives the same signal to both sides.

- **Bits 0–15: playback rate (Confirmed).** Read as a small float: e = bits 13–15, m = bits 0–12, rate = 48 kHz × 2^(e−5) × (1 + m/8192). This was checked against the measured DMA consumption for 41 distinct values. The mean ratio of measured to predicted was 1.000 with a 2% spread, which is the measurement noise, and no constants were fitted. Examples: `0x5acc` = 11,025 Hz, `0x7acc` = 22,050 Hz, `0xa000` = 48 kHz. The last is the value the reflection voice used in the test suite. Half-Life's values cluster around 11,025 Hz with a few percent spread, which matches the game's randomised pitch.
- **Bits 16–23: gain**, on the same log scale as the reflection gain (`0x10` per 6 dB). `0x00` is silent. New voices fade in over about 25 ms (`0x00` → `0x5c` → `0x6c` → `0x75` → `0x7b` …). Typical playing values are `0x8a`–`0x9c`, and the ceiling is `0xcf`.
- **Bits 24–31: treble low-pass**, most often `0xcf` (open), with lower values down to about `0xa4`. Measured on the reflection voice (run 14 above); the same law is assumed for other voices (Strong).

PARM2 was always 0. PARM3 is `0xcfef` in the high half, and its low half varies (`0xffff`, `0xdfbf`, `0xfffc` …). Its meaning is unknown.

**Test-suite run 11 (`wtpath`, Satellites) sorted out the two kinds of wavetable voice (Confirmed).**

- **Reflections are always first-half voice 0, playing at the source's own rate.** One click source with a reflective wall gave a rate field of `0xa000` for a 48 kHz source, `0x7acc` for 22.05 kHz and `0x5acc` for 11.025 kHz. At pitch 0.5 the field glided down to about `0x3b78`, roughly half. Gain stayed at `0xa0` and the top byte at `0xc6`, as in the earlier runs. The voice's DMA pages pointed into the same host buffer that the source's ADB channel was playing from. So the reflection is the source's own sample data, fetched a second time at a start offset the driver picks; that offset is the most likely place the reflection delay lives (Strong).
- **Second-half voices 0–3 feed A3D sources once ADB channels run out.** Sixteen sources were started 250 ms apart. The earlier ones took the free ADB channels (3–13, each feeding one A3D slot from 14 down to 4), and once those were used up the last four took second-half voices 0, 1, 2 and 3, one per source, each started at the same instant as its A3D source (slots 3, 2, 1, 0). Those voices ran at the source rate (`0x7acc`, 22.05 kHz) with gain and top byte fixed at `0xcf`. So a wavetable voice can stand in for an ADB channel as an A3D source's input, at unity gain.
- **Extra sources steal slots; they do not overflow to wavetable voices.** Each louder click added on top of the 16 quiet sources took over an existing A3D slot (slot 0, then 1, 2, 3, 4 …). No further wavetable voices were used.

So Half-Life's second-half voices 4–29 are neither reflections nor A3D overflow. They carry their own gain and filter and start independently of A3D sources, which fits ordinary (non-3D) sound buffers played on wavetable voices once the ADB channels are busy (Hypothesis). The route table did not change during play, so their routes were set when the driver loaded.

**Voice output routing (Strong, from the emulator).** A voice whose own ADB source (`0x40` + voice for the first half, `0x80` + voice for the second) appears in the route table plays only into that route, not into the half's mixed outputs (`WT0_32/33` and so on). `DSREG` (`0x0008` in each half) was 0 in every trace, so it cannot be what selects this. Modelling it this way was what made Half-Life's 3D sounds come out in the right places in 86Box: before the change, voices feeding A3D sources also leaked into the plain stereo mix.

### Clocks, interrupts and DMA

- **Master clock (Confirmed).** `SMP_TIME` (`0x29198`) counts at 12.288 MHz, which is 256 × 48 kHz (measured: 12,288,079 Hz against the host clock).
- **Timer (Confirmed).** Writing N to `0x2919c` schedules one timer interrupt N samples (N × 256 master clocks, N × 20.83 µs) later; it is one-shot, and the driver rewrites it in every interrupt. Over 16,000 periods, the card-time from the write to the next interrupt was 257–260 master clocks per unit, the excess being handler latency under QEMU. The Windows driver asks for about 2,345 samples (49 ms, about 20 interrupts per second) when idle and about 236 samples (4.9 ms, about 200 per second) while sound plays. Reading `0x2919c` returns bit 31 set when the timer has fired; the read acknowledges it.
- **Interrupt bits.** `IRQ_SOURCE` read `0x5000` at almost every interrupt (19,508 times). Bit 12 is the timer: the handler masks it in `IRQ_CTRL` (`0x31024` → `0x30024`) while servicing and unmasks it afterwards (Strong). Bit 14 is always set during operation and is not in the mask; its meaning is unknown. Bit 2 appeared in 7 of 19,515 reads; ALSA treats the low byte as error flags. The handler then writes 0 to `IRQ_SOURCE`.
- **ADB DMA.** Per channel: four page addresses (`BUFBASE`, `0x27400` + channel × 0x10), a configuration pair (`0x27800` + channel × 8), `CTRL` (`0x27a00` + channel × 4: offset bits 0–11, interrupt enable bit 12, direction bit 13, format bits 14–17) and status (`0x27e00` + channel × 4). The driver uses 4 KB pages (size field `0xfff` = size − 1) in a four-page ring and rewrites page addresses as playback advances. DMA interrupts stay disabled; the driver polls positions from the timer interrupt instead. Format codes 1 and 8 were seen; their meaning is unconfirmed.
- **Wavetable DMA** has the same structure at `0x27000`, `0x27600` and `0x27900`.

## Implications for an emulator

An emulated Vortex 2 can run Aureal's original Windows drivers and A3D API unmodified, so everything the driver computes on the host, which includes the HRTF filters, comes along for free. The device itself has to reproduce the chip, not the A3D algorithms. The model in `core/` (the same code as the 86Box device) does all of the following; it has no 86Box dependencies.

The model, in the order it was built:

1. **PCI device** with the IDs, BAR sizes and config-space defaults in PCI identity.
2. **Register file** of 256 KB with the reset values from the cold read, `0xDEADBABE` in decoded gaps, and last-value-read behavior from `0x2c000` up. Expose it through both BAR0 and the BAR1 index/data window.
3. **AC'97 codec** answering as a STAC9708/9711 through `CODEC_IO`.
4. **Interrupts, timer and DMA**: `IRQ_SOURCE`/`IRQ_CTRL`/`STAT`/`CTRL` at `0x2a000–0x2a00c`, the timer at `0x2919c`, the sample counter at `0x29198`, and the WT and ADB DMA engines at `0x27xxx` with their position and status registers.
5. **Audio path**: FIFOs, the ADB routing table (`0x28000`), sample-rate converters (`0x26xxx`), mixer with current/target volume ramps (`0x20000–0x21fff`), and the wavetable voices (`0x00000–0x0ffff`), which also carry reflections.
6. **A3D per source**: a 56-tap FIR per ear (right = upper halves, left = lower halves), an ITD delay, a gain and a biquad atmospheric filter, each gliding from current to target values. Then the speaker crosstalk canceller and hardware EQ.

Traces serve as a regression suite: `v2replay` replays a trace's register writes into the model and compares the model's answers to the driver's reads with what the card returned ([Appendix C](appendix-c-workflow.md)).

## Prior work

The Linux ALSA driver already names most of the chip; what no earlier project provided is verified behavior, timing and signal processing, which is what an emulator needs. No emulator implementation of the Vortex 2 turned up in our searches before the 86Box device.

| Project | What it covers | Limits |
| --- | --- | --- |
| [Linux ALSA `au88x0` driver](https://git.zx2c4.com/linux-dev/tree/sound/pci/au88x0/au8830.h) (Manuel Jander, 2003), grown out of the OpenVortex project (Jeff Muizelaar, Kester Maddock) | Register names for nearly every block, recovered from Aureal's `asp4core.vxd`; working playback, capture, mixer, EQ, crosstalk canceller, MIDI and game port; an A3D register map ([`au88x0_a3d.h`](https://git.zx2c4.com/linux-dev/tree/sound/pci/au88x0/au88x0_a3d.h)) | Many semantics are labelled guesses (for example `VOL_MAX`); the wavetable synth is unfinished; no timing or DSP arithmetic |
| [re-a3d-api](https://github.com/oxiKKK/re-a3d-api) (oxiKKK) | Reconstruction of Aureal's A3D library `a3dapi.dll` 3.3.677.0 and the A3D 1.x `a3d.dll` from the binaries, including the geometry engine, software renderer and the control record the library sends to Aureal's driver (see A3D engine) | Library level only: the Vortex chip's processing is not reconstructed and nothing has been run on a real card; no licence stated ("for study only") |
| [FreeBSD / DragonFly `au88x0`](https://gitweb.dragonflybsd.org/~tuxillo/dragonfly.git/blob/75b4da16fa23b21d85fd03c59414b1503a5bae46:/sys/dev/sound/pci/au88x0.c) (Dag-Erling Smørgrav, 2003) | Interrupt, control and routing registers for the 8820, 8830 and 8810 | Basic playback only |
| [OpenAureal](https://pt.osdn.jp/projects/freshmeat_openaureal/) | Fork of Aureal's own Linux (OSS) drivers | Partly binary-only |
| [A3D SDKs 1.2, 2.0, 3.0](https://www.github.com/sigmaco/a3d-sdk) (Aureal, archived) | API references and guides (A3D 2.0 as PDF), sample source including reflection and polygon samples, and retail, debug and debug-viewer builds of `a3dapi.dll` 2.0 and 3.0; the debug builds carry assertion text and internal source file names | No hardware material: the Vortex data sheets and product briefs were "available only on the SDK CD" and are not in the archive |
| ZanQuance's driver and API revival ([VOGONS](https://www.vogons.org/viewtopic.php?p=475923)) | Reports that WaveTracing runs in `A3DAPI.dll` and feeds the wavetable engine, and that OPL3 is emulated in software ([thread](https://www.vogons.org/viewtopic.php?p=432820)) | No published register documentation |
| A3D-Live, Creative ALchemy, [DSOAL](https://github.com/kcat/dsoal) | Replace the A3D or DirectSound API in software | No hardware model |

## Open questions

The major gaps, roughly in order of what an emulator needs first:

- [ ] **Timer and interrupts**: the timer (N samples, one-shot), master clock (12.288 MHz), DMA page ring and timer interrupt bit are now known. Still open: `IRQ_SOURCE` bits 14 and 2, and how the chip behaves if the driver services an interrupt late.
- [x] **The routing graph** the Windows driver builds for WAV and A3D playback (see Routing). Still to capture: recording, S/PDIF and DOS modes.
- [ ] **Sample-rate converter internals**: `CONVRATIO`, Doppler, pitch and DMA format codes are decoded, and the filter response is measured (run 12). The `U0–U3` and drift words are not decoded, and conversion from input rates above 48 kHz is untested.
- [x] **A3D arithmetic**: done (test-suite runs 1–4, the glidewatch readback and run 12). Gain and taps are Q15 at unity scale, ITD is in 1/512-sample units, and the atmospheric filter is a double real pole. The direction table (15° × 15°) is captured, and the driver interpolates it bilinearly in azimuth and elevation: 351 positions on a 3.75° grid in three regions matched bilinear interpolation of the table to within 0.05% (rounding), against 7–16% for nearest-point or one-axis schemes. Glides step every 64 samples with time constants 2²¹ / TC (gain, filter, HRTF) and 2¹⁹ / TC (ITD).
- [ ] **Wavetable engine**: the PARM0/PARM1 rate, gain and filter bytes are decoded (see Wavetable voices in a game). Still open: PARM3, the per-voice words at `0x8080`/`0x8180`, `0x8008`, the voice output routing, and what decides which sounds the driver puts on wavetable voices.
- [ ] **Crosstalk canceller**: working empirical model for all three speaker settings (validated on an independent session), structure known, EQ block decoded. Still open: the register-level arithmetic, which only matters for software that loads its own coefficients (none known besides Aureal's driver). Run 11 repeated the 25-step coefficient test while re-arming the canceller (`0x24660` bit 0 off, then on) after every change. The responses matched run 10's to within about 0.1 in band ratio, so new coefficients take effect at once and the latch bit is not needed. With the coefficient layout we assumed, changing b0 acts like a gain change, but changing b1, b2, a1 or a2 in a section moves the output by less than about 1 dB. Either those words are elsewhere in the 9-word section, or the arithmetic is not a plain biquad.
- [x] **True power-on state**: read after a full power-off (2026-10-30); memory regions power up random, fixed patterns and control registers as listed under Address map.
- [ ] **Legacy DOS support**: Sound Blaster in a DOS window is software emulation by the Windows driver over the normal playback path (see DirectSound buffers and the mixer). MIDI through the "Wavetable" device is host software synthesis (run 12). Still open: real-mode DOS, the MPU-401 device, the game port, and what BAR2 is for.
- [ ] **Coverage**: other driver versions (such as 2041), Windows 2000 and XP drivers, A3D 1.0 titles, two-channel boards (SQ1500, Montego II), S/PDIF and the SQ3500 DSP daughterboard.

## References and data

**Source code and documentation**

- Linux ALSA `au88x0` driver: [`au8830.h`](https://git.zx2c4.com/linux-dev/tree/sound/pci/au88x0/au8830.h) (register names, `NR_A3D`, `CODEC_IO` format), [`au88x0_a3d.h`](https://git.zx2c4.com/linux-dev/tree/sound/pci/au88x0/au88x0_a3d.h) (A3D layout), [`au88x0_core.c`](https://android.googlesource.com/kernel/common/+/673a7341bdab66287caed9f6233e45650d9e1094/sound/pci/au88x0/au88x0_core.c) (route lists, DMA, mixer), [`au88x0_xtalk.c`](https://android.googlesource.com/kernel/common/+/073815b756c51ba9d8384d924c5d1c03ca3d1ae4/sound/pci/au88x0/au88x0_xtalk.c) and [`au88x0_eq.c`](https://android.googlesource.com/kernel/common/+/073815b756c51ba9d8384d924c5d1c03ca3d1ae4/sound/pci/au88x0/au88x0_eq.c).
- [re-a3d-api](https://github.com/oxiKKK/re-a3d-api): reconstruction of Aureal's A3D library; source of the `A3DCTRL_SRC_SUPER` control record (`src/ia3ddal.h`).
- [A3D SDKs 1.2, 2.0, 3.0](https://www.github.com/sigmaco/a3d-sdk): API documentation, samples and library builds.
- [Phil's Computer Lab: Aureal Vortex 2](https://www.philscomputerlab.com/aureal-vortex-2.html): driver packages, including 4.06.2048.

**Tools and data in this repository**

| Path | Purpose |
| --- | --- |
| `scripts/vortex-session.sh` | One command: sets the line-in gain, records the card's output, boots the traced VM, logs all timing |
| `tools/vortex_trace.py` | Raw QEMU log to register timeline and per-register summary |
| `tools/a3d_params.py`, `tools/a3d_slices.py` | Per-source A3D parameters and coefficient statistics from a timeline |
| `tools/vortexprobe.py` | PCI identity, cold and running register snapshots, snapshot diffs, codec decoding, routing/SRC/DMA decoding, sample-clock measurement |
| `tools/vortex-poke.py` | Host-side register reads and writes while the VM runs (glide, canceller and EQ sweeps) |
| `tools/a3dtest/` | Win98 test program that drives the A3D and DirectSound APIs one setting at a time and logs every step; media script for its CD image and results disk |
| `tools/sbtest/` | Minimal Sound Blaster test for a DOS window |
| `tools/v2replay.c`, `core/` | The chip model and a trace replayer |
| `tools/hrtf_extract.py`, `tools/make_mhr.py` | Read the driver's direction table out of a traced run; turn it into an OpenAL Soft HRTF set |
| `data/xtalk/` | Crosstalk-canceller model |

**Measurement sessions**

The traces and recordings themselves are not in the repository (they are large and the recordings contain Aureal's demo audio and game sound). The sessions referred to above:

| Session | Contents |
| --- | --- |
| A3D circling demo | Helicopter and fountain orbiting the listener; trace only |
| A3D distance demo | Sources moved toward and away; trace only |
| `20261030-215702-distance-audio` | Boot, helicopter and fountain WAVs in the player demo; trace plus aligned line-in recording |
| Cold read | 256 KB register snapshot after power-on reset |
| `20261030-222820` | Running snapshots: idle, WAV playing, A3D demo; trace plus recording |
| PCI probe | Config space, BARs, board and BIOS details |
| `heli-baseline`, `heli-reflections-off`, `heli-refl-delay`, `heli-occlusions-off`, `heli-ds3d` (actually Stereo mode) | Helicopter demo with one setting changed per run; trace plus recording |
| `20261030-231116-rooms-occlusion` | A3D Rooms, walking so walls pass between listener and sources; 15 sources active |
| `20261030-232006-heli-reflec2` | Reflections on, reflection delay and Doppler moved stepwise |
| 20261031-093348-a3dtest | Test suite run: 154 logged steps (formats, azimuth, elevation, distance, gain, eq, Doppler, pitch, occlusion, reflections, glide); trace, aligned line-in recording and step log |
| 20261031-142309-a3dtest2 | Test suite run 2: output modes, reference clicks, 670-position direction map, single-parameter glides, reflection sweeps, 16 simultaneous sources, DirectSound formats, volume, pan and frequency; trace, recording and step log. |
| 20261031-145906-a3dtest3, 20261031-151126-a3dtest3-2, 20261031-163240-a3dtest4 | Headphone mode (set in Aureal's control panel): the run-2 suite with the canceller off; 5° elevation sweep; 36-cycle glide test at the driver's time constants; the same glide test with each time-constant word rewritten from the host (vortex-poke.py log kept with the session) |
| Runs 5–11, 2026-10-31 (including 20261031-191956-duke-dos) | Four-speaker modes, canceller and EQ sweeps from the host, wavetable path (`wtpath`), Duke Nukem 3D setup in a DOS window |
| 20261031-203721-hl-play2 | Five minutes of Half-Life in A3D mode |
| 20261101-165822-glide | Glide readback: target changes from the host with the current registers polled every 100 µs |
| 20261101-175550-sbpro | Sound Blaster test program in a DOS window |
| 20261101-185801-run12 | Run 12: converter response, levels and headroom, MIDI, bilinear direction grid |
