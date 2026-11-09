# Appendix C: measuring and testing

How the measurements in the [reference](reference.md) were made, so that anyone with a card can repeat or extend them. Everything here runs on Linux, with the card passed to a Windows 98 virtual machine. Nothing on the card is emulated during a measurement: Aureal's driver talks to real silicon, and QEMU logs every access.

## What you need

- An Aureal Vortex 2 card (SQ2500 or another AU8830 board) in a PCI slot that VFIO can pass through on its own. The measurements here used an ASUS Sabertooth X79, whose legacy PCI slot hangs off the chipset in its own IOMMU group.
- An IOMMU enabled in firmware and on the kernel command line (`intel_iommu=on` or `amd_iommu=on`), and the card bound to `vfio-pci`.
- QEMU with VFIO support (11.1.2 was used), and a Windows 98 SE disk image with Aureal's driver installed (4.06.2048 was used; [Phil's Computer Lab](https://www.philscomputerlab.com/aureal-vortex-2.html) has the packages).
- A line input on the host to record the card's output (any motherboard input will do; the analysis compares recordings with each other, so the input's own response cancels out).
- Python 3 with NumPy for the analysis scripts; `pw-record` or `arecord`; `amixer`; for the test media, `genisoimage`, `dosfstools` and `mtools`.

## A traced session

`scripts/vortex-session.sh` does the whole thing in one command:

```sh
./scripts/vortex-session.sh list-inputs          # find the line input
./scripts/vortex-session.sh test-rec 5           # check levels and that both channels arrive
./scripts/vortex-session.sh run LABEL            # record + boot the traced VM; shut Windows down to finish
./scripts/vortex-session.sh mark "TEXT"          # from a second terminal: timestamp a marker
```

It sets the line input's gain (Line Boost off, Capture at `CAPTURE_DB`, default 0 dB), starts recording, boots the VM with the card passed through and register tracing on, and writes everything into one session folder: `qemu-trace.log`, `audio.wav`, `session.json` (every start and stop time in UTC, on the same clock as the trace), `markers.tsv`, `mixer.txt`, and, after parsing, `timeline.txt` and `summary.txt`. Settings are environment variables listed at the top of the script; the ones most often changed are `REC_TARGET`, `CAPTURE_DB`, `EXTRA_CD` (an ISO to attach) and `EXTRA_DISK` (a raw disk that keeps its changes, used for test results).

The tracing relies on two QEMU details. The card is attached with `-device vfio-pci,host=…,x-no-mmap=on`, so the guest cannot map the card's registers directly and every access goes through QEMU; and the `vfio_region_read` and `vfio_region_write` trace events are logged with timestamps (`-trace … -msg timestamp=on`). Tracing slows the driver a little but does not change what it does.

`session.json` gives the recording's start time relative to the trace's first access (`audio_seconds_at_trace_t0`), good to a few tens of milliseconds. For finer alignment, use a sound with a sharp start: `a3dtest` begins and ends every run with three clicks for this.

## Reading a trace

```sh
tools/vortex_trace.py summary qemu-trace.log           # per-register counts, first/last time, sample values
tools/vortex_trace.py timeline qemu-trace.log OUT.txt  # one line per access, repeats collapsed
tools/vortex_trace.py csv qemu-trace.log OUT.csv
```

The driver reaches the registers both through BAR0 and through the BAR1 index/data window; the parser folds both into one stream of `time op register value`. `tools/a3d_params.py` and `tools/a3d_slices.py` pull the per-source A3D parameters out of a timeline.

## Probing the card

`tools/vortexprobe.py` (run with `sudo`):

| Command | What it does |
| --- | --- |
| `pci` | PCI identity, config space, BARs, IRQ |
| `cold` | Reads all 256 KB of registers from Linux before any driver has run (do this after a full power-off for the true reset state) |
| `snap NAME` | Reads all registers from the running VM through QEMU's QMP socket |
| `diff A.bin B.bin` | Compares two snapshots |
| `decode SNAP.bin` | Routing graph, converter rates, DMA set-up, mixer volumes |
| `codec TIMELINE` | AC'97 codec identity and register settings from a trace |
| `clock` | Measures the card's sample clock against the host |

`tools/vortex-poke.py` reads and writes registers from the host while the VM runs, through the card's `resource0` in sysfs. Host writes do not appear in the QEMU trace, so it logs each one with a UTC timestamp. Its sweeps are meant to run alongside particular `a3dtest` tests: `tcsweep` and `glidewatch` with `A3DTEST tcsweep` (the glide law in the reference came from `glidewatch`, which polls the chip's current registers every 100 µs after each target change), `xtsweep`, `xtident` and `xtident2` with `A3DTEST click90` (the crosstalk canceller), and `eqsweep` with `A3DTEST noise2d` (the EQ block). Each sweep puts back every word it changed.

## The test program

`tools/a3dtest` is a Windows 98 program that drives the A3D 2.0 and DirectSound APIs one setting at a time, holds each setting long enough to measure, and logs each step with a timestamp to `A3DTEST.LOG` on the results disk. It builds with a MinGW cross compiler and needs `ia3dapi.h` from Aureal's A3D 2.0 SDK, which is not redistributed here (it is in the [archived SDKs](https://github.com/sigmaco/a3d-sdk)):

```sh
git clone https://github.com/sigmaco/a3d-sdk tools/a3dtest/a3d-sdk
make a3dtest                                          # or: cd tools/a3dtest && ./build.sh
tools/a3dtest/a3dtest-media.sh iso                    # CD image with A3DTEST.EXE
tools/a3dtest/a3dtest-media.sh disk                   # empty FAT16 results disk
EXTRA_CD=~/a3dtest/a3dtest.iso EXTRA_DISK=~/a3dtest/results.img ./scripts/vortex-session.sh run LABEL
#   in Windows: D:\A3DTEST.EXE run12   (or whichever tests; see the list below)
tools/a3dtest/a3dtest-media.sh log                    # read the log back
```

Run 15 also needs the host side. Start it in a second terminal before running the guest program; it waits for the two tests it acts in and logs its writes, with UTC times, to `poke-log.jsonl`:

```sh
sudo tools/vortex-poke.py run15 --log ~/a3dtest/run15-poke.jsonl
#   in Windows: D:\A3DTEST.EXE run15   (about 5 minutes)
```

The log's clock is the guest's and drifts slightly against the trace. Line steps up with the trace by the opening and closing sync clicks, or by bursts of register writes (each change of position, for instance, produces one burst of HRTF writes).

| Test | Measures |
| --- | --- |
| `sync` | Three clicks for alignment (run at the start and end of every suite) |
| `formats`, `dsound` | Converter ratios and DMA formats at each rate and format; DirectSound volume, pan and frequency |
| `azimuth`, `elevation`, `hrtfmap`, `elev5`, `hrtfgrid` | The direction table and its interpolation |
| `distance`, `gain`, `eq`, `occlusion` | Gain and the atmospheric filter |
| `doppler`, `pitch` | Converter ratio changes |
| `reflections`, `reflect2`, `wtpath` | Reflections and the wavetable path |
| `glide`, `glide2`, `tcsweep` | Glides from current to target values |
| `xtalk`, `ref`, `click90`, `noise2d` | Output modes, reference levels, canceller and EQ stimuli |
| `multi` | Sixteen simultaneous sources (slot allocation) |
| `srcresp`, `level`, `midi` | Converter filter response, levels and headroom, the MIDI synthesiser |
| `reflhf` | Reflection tone and level against wall reflectance |
| `dsfill`, `dshold`, `dswt`, `ds3d`, `dsstream`, `eqhold` | DirectSound buffers on wavetable voices: where 24 buffers land and whether each is heard; volume, pan, frequency, stereo and 8-bit on a voice against the same tone on an ADB channel; DirectSound 3D buffers; a streaming buffer; steady stimuli for `vortex-poke.py run15` (`DSREG` and EQ) |

Suites: no arguments runs the run-2 set; `v1`, `run3`, `run12`, `run13` and `run15` run those sessions' sets; `all` runs everything. The comment at the top of `a3dtest.c` lists the exact contents.

`tools/sbtest/sbtest.asm` is a small DOS program (assemble with `nasm -f bin sbtest.asm -o SBTEST.COM`) that resets the Sound Blaster DSP at 220h, reads its version, plays one second of 8-bit DMA on DMA 1 and waits for IRQ 5, printing each step. Real-mode DOS cannot be tested under QEMU passthrough, because QEMU does not forward legacy ISA ports to the card; in a Windows DOS window the driver's software emulation answers.

## Checking a model against a trace

`bin/v2replay` (`make`) replays a timeline's register writes into the chip model in `core/` at the traced times, and compares the model's answers to the driver's reads with what the card returned:

```sh
bin/v2replay TIMELINE                 # compare the timing-independent registers
bin/v2replay -a -v -t 60 TIMELINE     # compare every read for the first minute, listing mismatches
bin/v2replay -o out.raw TIMELINE      # also write the model's output (48 kHz, stereo, s16)
```

The trace holds no guest memory, so the replay feeds DMA a fixed noise pattern; the output shows how the chip routes and processes streams, not what was played. A session traced in 86Box instead (the device's register-trace setting) also records the fetched sample data, and `-d vortex2-dma.bin` plays that back, so the replay reproduces the emulator's audio exactly; `-s a6,a7` then writes one source pair (here 3D slice 0's outputs) instead of the mix, to hear each path on its own. With `-a`, position and status registers differ because the replay does not follow the guest's timing, and memory regions the driver never writes differ because they power up random; the rest should match.

For audio comparisons, record the same test in the emulator (86Box's application audio capture, or the diagnostic log described in [using-86box.md](using-86box.md)) and compare with the session's `audio.wav`. The comparisons in the reference compare recordings with each other (for example each converter rate against 48 kHz), so the line input's own response drops out.

## Safety

Passing a sound card through is low-risk: in all the sessions here, the worst outcome was a guest that had to be restarted. Run the VM with `-snapshot` (the session script's default) so that driver experiments do not change the disk image, and use a full power-off when you need the card's true reset state, since a warm reboot leaves its memory-like regions intact.
