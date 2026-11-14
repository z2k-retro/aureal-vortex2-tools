# aureal-vortex2-tools

Documentation, data and tools for the **Aureal Vortex 2** (AU8830), the sound chip on the Aureal SQ2500 and other A3D 2.0 cards (1998). They were made to build and verify the Vortex 2 emulation in [86Box](https://86box.net), developed on the [`vortex2` branch of z2k-retro/86Box](https://github.com/z2k-retro/86Box/tree/vortex2). With them, a real card can be traced while Aureal's driver runs, its output recorded and measured, and an emulator checked against both.

**Just want to try it?** See [Using the Vortex 2 emulation in 86Box](docs/using-86box.md).

The main finding behind the emulation: Aureal's Windows driver computes the HRTF filters, reflections and MIDI synthesis on the host and sends finished results to the chip. An emulator that runs the original driver therefore needs to reproduce the chip's fixed signal path (filters, delays, glides, converters, mixer, canceller), not Aureal's 3D algorithms, and the [reference](docs/reference.md) documents that path from measurement.

## What's here

| Path | What it is | Needs a card? |
| --- | --- | --- |
| [`docs/using-86box.md`](docs/using-86box.md) | User guide to the 86Box emulation | No |
| [`docs/`](docs/README.md) | Technical reference: registers, the A3D engine, routing and timing, measured behaviour, procedures | — |
| `core/` | The chip model, the same code as the 86Box device; no 86Box dependencies | No |
| `tools/v2replay.c` | Replays a traced session into the model and checks every register read against the card | No |
| `data/xtalk/` | The crosstalk-canceller model | No |
| `tools/hrtf_extract.py`, `tools/make_mhr.py` | Read the driver's direction table out of a traced run; turn it into an OpenAL Soft HRTF set | Yes (to capture) |
| `tools/vortex_trace.py`, `tools/a3d_params.py`, `tools/a3d_slices.py` | Turn QEMU traces into register timelines and A3D parameters | No |
| `tools/vortexprobe.py` | PCI identity, register snapshots (cold and running), decoding of routing, converters, DMA and codec | Yes |
| `tools/vortex-poke.py` | Register reads, writes and sweeps from the host while the VM runs | Yes (VFIO) |
| `tools/a3dtest/` | Windows 98 test program that drives the A3D and DirectSound APIs one setting at a time and logs each step | Yes |
| `tools/sbtest/` | Minimal Sound Blaster test for a DOS window | Yes |
| `scripts/vortex-session.sh` | One command for a traced session: records the card's output and boots the Windows 98 VM with every register access logged | Yes (VFIO) |

## No card? You can still help

- **Play games in the 86Box build** and report what sounds wrong, with the diagnostic log. The [user guide](docs/using-86box.md#reporting-a-problem) says what to include.
- **Build an A3D wrapper that sounds like the card.** The [crosstalk model](docs/appendix-b-crosstalk-model.md) is here, and [Appendix A](docs/appendix-a-direction-table.md) describes the driver's direction table and how a card owner can read it out.
- **Work on the model without hardware.** `core/` is plain C with a small host interface (`core/snd_vortex2_core.h`), and everything it models is described in the [reference](docs/README.md).
- **Card owners**: the open questions at the end of the [reference](docs/reference.md#open-questions) list what is still unmeasured: recording, the game port and MPU-401, real-mode DOS, other driver versions and boards (SQ1500, Montego II, SQ3500), and the canceller's arithmetic. [Appendix C](docs/appendix-c-workflow.md) walks through every procedure.

## Building

```sh
make            # bin/v2replay
make a3dtest    # tools/a3dtest/a3dtest.exe (MinGW cross compiler and the A3D 2.0 SDK header; see Appendix C)
```

`v2replay` builds anywhere with a C99 compiler. The Python tools need Python 3 (and NumPy for the analysis); the card tools need root, an IOMMU, and the card bound to `vfio-pci`.

## Not included

These are not in the repository, because they are not ours to distribute or are too large:

- Aureal's drivers, SDK headers and demos. The drivers are on [Phil's Computer Lab](https://www.philscomputerlab.com/aureal-vortex-2.html); the SDKs are [archived on GitHub](https://github.com/sigmaco/a3d-sdk).
- The traces and recordings themselves. They are large, and the recordings contain Aureal's demo audio and game sound; capture your own with `scripts/vortex-session.sh`.

## Licence and credits

GPL-2.0-or-later (see `LICENSE`), to match 86Box. The canceller model is a measurement of the card's response under Aureal's driver. The driver's direction table is included (`data/hrtf/`): the table as captured from a real SQ2500, and the OpenAL Soft HRTF set built from Aureal's "Ed's Ears" profile, which is the same table with lower elevations added; Appendix A explains how it was read out.

Hardware, testing and direction: Z2k Retro. The tools, chip model and documentation were written with substantial help from Claude, Anthropic's AI model. Statements marked as measured were checked on a real SQ2500 by the methods in Appendix C; the rest come from the Linux driver or from the 86Box model's behaviour, as the reference says for each.
