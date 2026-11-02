# Appendix A: the driver's direction table

Aureal's Windows driver (4.06.2048) turns a direction into HRTF filter taps and an interaural delay (ITD) by looking them up in a table and interpolating. The A3D engine on the chip only runs the finished filters ([reference](reference.md#a3d-engine)), so this table is what gives the card its sound. An emulator that runs Aureal's driver gets it from the driver; anything that replaces the driver (an A3D wrapper such as DSOAL's, or a game port) needs it.

The table's contents are Aureal's filter data and are not published here. This appendix describes its structure, and how anyone with a card and the driver can read it out for their own use, for example to make an A3D wrapper sound like the card.

## What was measured

The table was read out of the driver by placing one A3D source at known directions and recording the HRTF target and ITD words the driver wrote to the chip (test-suite runs 2, 3-2 and 12; see [Appendix C](appendix-c-workflow.md)). Every value repeated exactly across runs.

- **Grid.** One entry every 15° of azimuth and 15° of elevation, from −45° to +75°, plus one entry straight up: 217 entries. Directions below −45° were not sampled.
- **Interpolation.** For any other direction the driver blends the four surrounding entries bilinearly in azimuth and elevation, taps and ITD alike. Run 12 checked 351 positions on a 3.75° grid in three regions (around the front, below and to the right, and above and behind): all matched bilinear interpolation to within rounding (0.05%).
- **Straight up** gives the same taps at every azimuth.
- **Distance** does not change the taps or ITD; it is handled by the gain and the atmospheric filter.
- **180°** is an outlier at every elevation: about twice the tap energy of its neighbours, and a 40-sample left-ear ITD from −15° to +30°. Directions from 165° to 195° interpolate toward it.

## Coordinates and units

- Azimuth is clockwise from straight ahead (90° = right); elevation is up from the horizontal plane. In `a3dtest`'s coordinates the listener is at the origin facing −Z, with +X to the right and +Y up.
- Each entry is 56 taps per ear, signed 16-bit, Q15 (`0x7fff` = 1.0), at 48 kHz. On the chip, tap *k* of both ears shares one word: right ear in the upper half, left ear in the lower.
- The ITD word holds the left-ear delay in its lower half and the right-ear delay in its upper half, in units of 1/512 sample (`0x200` = one sample at 48 kHz). Only the far ear is delayed.

## Its general shape

- The ITD follows a spherical-head (Woodworth) curve: about 7 samples at 15°, 19 at 45°, 35 at 90°, and a maximum of 37–38 samples (about 0.78 ms) just behind the side. Elevation shrinks it (at 90° azimuth: 22 samples at 45° up, 0 straight up).
- The right/left energy balance of the taps swings between about +15 dB and −16 dB as a sound circles the listener; straight up it is +1.1 dB.
- The left and right halves are separate measurements, not mirror images: at 90° the near-ear taps correlate 0.87 with the mirrored 270° near-ear taps.

## Reading it out yourself

With a card, Aureal's driver and the traced setup in [Appendix C](appendix-c-workflow.md):

1. Run a session with `A3DTEST hrtfmap` (every 5° of azimuth at each 15° elevation, about three minutes). `elev5` and `hrtfgrid` add finer elevation steps and off-grid points.
2. Make the timeline: `tools/vortex_trace.py timeline qemu-trace.log timeline.txt`.
3. Extract the directions: `tools/hrtf_extract.py timeline.txt A3DTEST.LOG table.csv --test hrtfmap --grid`. It matches each burst of target writes to the log's steps and writes one row per direction: `az_deg`, `el_deg`, `itd_word`, `itd_left_samples`, `itd_right_samples`, then `left0`–`left55` and `right0`–`right55` as signed integers.
4. Optionally, `tools/make_mhr.py table.csv vortex2.mhr` turns the grid into an OpenAL Soft HRTF data set (MinPHR03, 48 kHz, 56 taps, separate ITDs; elevations below −45° repeat the −45° entries). OpenAL Soft also blends the four nearest grid points bilinearly, so it follows the card's interpolation.
