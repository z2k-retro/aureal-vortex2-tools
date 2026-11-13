# Appendix B: crosstalk-canceller model

The Vortex 2 has a speaker crosstalk canceller at `0x24000–0x25fff` between the A3D engine and the mixer ([reference](reference.md#reflections-stereo-mode-and-speaker-output)). For loudspeakers it feeds each side an inverted, delayed, low-passed copy of the other side, so that the HRTF cues reach each ear without being smeared by the opposite speaker. Aureal's control panel picks one of three coefficient sets, word for word the `Wide`, `Narrow` and `Pipe` tables in ALSA's `au88x0_xtalk.c`:

| Control panel setting | ALSA table | Effect |
| --- | --- | --- |
| Satellite speakers (the default) | `Wide` | Crosstalk 1.04 / 0.63 / 0.31 at 225 Hz / 900 Hz / 3.6 kHz, inverted, about 0.45 ms |
| Monitors | `Narrow` | 1.02 / 0.75 / 0.53, inverted, about 0.25 ms |
| Headphones | `Pipe` | Pass-through |

The register-level arithmetic of the canceller's filter sections is only partly decoded. Because only these three presets are ever loaded, the canceller can be emulated by its measured response instead. That is what the 86Box model does, and what these files provide.

### Partial decode (Plausible)

Aureal's description of the block, relayed by the author of a register map of the chip, gives it two 10th-order IIR filters per side built from second-order sections: an "EQ" filter and an "XT" filter, the XT filter preceded by a 32-stage ITD delay line (the control word at `0x24660` carries a 5-bit delay per side; the driver sets 13 samples for satellites). The sections are double-precision direct-form I biquads with error feedback, on a 16 × 16-bit multiplier and 40-bit accumulator (12.28); inputs and delayed inputs are 1.15, delayed outputs 2.14, and each state is held as a 4.12 upper and an x.14 lower word. The RAM holds, per section, five coefficient words and four state words (y1, y2 and two error words), ten EQ sections at `0x24200` and ten XT sections at `0x24368`, ten input gains at `0x244d0`, and a gain (k) and shift per filter per side at `0x24538`–`0x24554`.

The driver's satellite preset fits that description. The same five sections are loaded for each side; every section's fifth word is zero, which is what a parallel (partial-fraction) form has (first-order numerators), not a cascade; the fifth XT section is empty; the first four input gains per side are just under 1 (2.14) and the fifth is 0; the EQ gain is positive with a shift of 1 and the XT gain negative with a shift of 0. Read as coefficients a1, a2, b0, b1 in 2.14 with poles 1 − a1 z⁻¹ − a2 z⁻² (all stable), the EQ sections summed in parallel give the same-side shape the model was fitted to, peaks and dips at the same frequencies, but only after adding a direct term of about 0.79 × the input to the gain-scaled sum: with it the shape matches the fitted filter to 0.67 dB rms from 150 Hz to 15 kHz (2.2 dB without). Where that term comes from in the hardware is not known. The same description gives the difference equation y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] + a1·y[n−1] + a2·y[n−2], gains and tracking rates as 1.15 fractions, no feedback between the sides, a dry (bypass) mix in each block, and a final 2× left shift. The cross path decodes on that basis in both presets the driver loads: the XT sections, summed in parallel and scaled by the XT gain, match the fitted cross filter (the ratio of cross to same-side response) to 0.72 dB rms from 150 Hz to 3 kHz for satellites and 0.49 dB for monitors (the monitors preset: XT delay 7 samples, EQ shift 3, input gains 0x7fff). Above 3 kHz they differ more (9 and 2 dB), where the fitted filters are limited by the recordings, so the decoded filters may be the better description there. So the canceller has the form the model already uses, out_L = E·(in_L + X·delay(in_R)), with X the XT filter. The same side (E) decodes for satellites (0.67 dB rms) with a dry term of about 0.79 × the input beside the gain-scaled EQ sum, but not yet for monitors (3.9 dB rms at best), and the dry gain's register is not identified: the driver never writes the Mixin words. Open: the dry gain, how the EQ shift and input gains combine with it, and the monitors same side.

## The model

Per ear:

    out_L = E * (delay(in_L, 64) + X * in_R)
    out_R = E * (delay(in_R, 64) + X * in_L)

`E` (the per-ear EQ) and `X` (the crosstalk path) are 512-tap FIRs at 48 kHz, fitted separately for Wide and Narrow. Each carries a 64-sample bulk latency, so the model adds 128 samples (2.7 ms) in all; the real chip's latency was not measured, and an emulator may trim it. `E` is normalised to 0 dB on average between 300 Hz and 3 kHz. **The real canceller is 5.5 dB (satellites) and 6.2 dB (monitors) below that (Strong).** Minerva's DirectSound 3D tests on a real card and in 86Box with headphones, with the same source and the same HRTF taps from the driver, give the real canceller's response for a centred source. Against the normalised model it is lower by those amounts, flat within about 1.5 dB from 200 Hz to 10 kHz (24 half-octave bands over two tests per setting). Run 2's tap fit (about −6.5 dB, satellites) agrees. The 86Box model applies them. Those levels were measured with the driver's `0x0a` on the canceller's mixer inputs taken as +1.5 dB; since the model moved to the exponent/mantissa mixer law (`0x0a` = +1.94 dB), it trims the canceller output by 0.44 dB so the measured end-to-end levels stay as they were. Headphones are a straight pass-through.

The filters were fitted on clicks recorded at seven positions per mode (runs 2 and 5) and tested on run 1, an independent session with 44 click positions. The model's error there is 4.5–10% below 2.4 kHz and 19–28% above, where the recording itself is the limit; without crosstalk the error is 33–80%.

## Files

| File | Contents |
| --- | --- |
| `data/xtalk/vortex_xtalk_fir.h` | The four FIRs as C arrays (`xt_wide_E`, `xt_wide_X`, `xt_narrow_E`, `xt_narrow_X`), `XT_TAPS` and `XT_LATENCY` |
| `data/xtalk/vortex_xtalk_fir.npz` | The same filters for NumPy |
| `data/xtalk/vortex_xtalk.py` | Reference implementation: `process(in_l, in_r, mode)` with mode `wide`, `narrow` or `headphones` |

`core/snd_vortex2_core.c` applies the same filters by FFT convolution, with partitioned FFT convolution. It treats the canceller as a pass-through (headphones) when the driver leaves the gain words at zero, and otherwise picks Wide or Narrow from the delay field of `0x24660` (`0xd` or `0x7`).

## The EQ block

The chip's ten-band EQ at `0x2b000` is a separate block in the 2D path and is left transparent by the Windows driver; the [reference](reference.md#reflections-stereo-mode-and-speaker-output) describes the band model for software that programs it.
