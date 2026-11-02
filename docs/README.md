# Aureal Vortex 2 technical reference

What an Aureal Vortex 2 (AU8830, as on the SQ2500) does, and how Aureal's Windows 98 driver drives it. It is written to be enough to emulate the chip or to replace its driver. Facts come from three places, and each section says which:

- the Linux ALSA `au88x0` driver, which names most registers;
- measurement of a real SQ2500 under Aureal's driver 4.06.2048, with every register access traced and the output recorded;
- host-side experiments that read and write the card's registers directly.

Each finding is tagged **Confirmed**, **Strong** or **Hypothesis** by how well it is established.

## Documents

- **[using-86box.md](using-86box.md)**: user guide to the 86Box emulation (setup, settings, limitations, reporting problems).
- **[reference.md](reference.md)**, the main document:
  - PCI identity, the host interface and the address map, with reset values;
  - the AC'97 codec;
  - the A3D engine: memory layout, HRTF filters, gain and atmospheric filter, glides, the driver's conversions from API settings, reflections, the crosstalk canceller, four-speaker output and the EQ;
  - routing, sample-rate conversion, DirectSound buffers, Sound Blaster and MIDI, wavetable voices, clocks, interrupts and DMA;
  - what an emulator needs, prior work, and open questions.

## Appendices

| | Appendix | Contents |
| --- | --- | --- |
| A | [Direction table](appendix-a-direction-table.md) | The structure of the driver's HRTF and ITD table, its interpolation, and how to read it out |
| B | [Crosstalk-canceller model](appendix-b-crosstalk-model.md) | The measured model of Aureal's three speaker presets |
| C | [Measuring and testing](appendix-c-workflow.md) | Traced sessions, probes, the test program and trace replay, step by step |
