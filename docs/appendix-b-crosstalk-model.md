# Appendix B: crosstalk-canceller model

The Vortex 2 has a speaker crosstalk canceller at `0x24000–0x25fff` between the A3D engine and the mixer ([reference](reference.md#reflections-stereo-mode-and-speaker-output)). For loudspeakers it feeds each side an inverted, delayed, low-passed copy of the other side, so that the HRTF cues reach each ear without being smeared by the opposite speaker. Aureal's control panel picks one of three coefficient sets, word for word the `Wide`, `Narrow` and `Pipe` tables in ALSA's `au88x0_xtalk.c`:

| Control panel setting | ALSA table | Effect |
| --- | --- | --- |
| Satellite speakers (the default) | `Wide` | Crosstalk 1.04 / 0.63 / 0.31 at 225 Hz / 900 Hz / 3.6 kHz, inverted, about 0.45 ms |
| Monitors | `Narrow` | 1.02 / 0.75 / 0.53, inverted, about 0.25 ms |
| Headphones | `Pipe` | Pass-through |

The register-level arithmetic of the canceller's filter sections is not decoded: host-side sweeps showed the sections are not a plain biquad cascade with any ordering or scale that was tried. Because only these three presets are ever loaded, the canceller can be emulated by its measured response instead. That is what the 86Box model does, and what these files provide.

## The model

Per ear:

    out_L = E * (delay(in_L, 64) + X * in_R)
    out_R = E * (delay(in_R, 64) + X * in_L)

`E` (the per-ear EQ) and `X` (the crosstalk path) are 512-tap FIRs at 48 kHz, fitted separately for Wide and Narrow. Each carries a 64-sample bulk latency, so the model adds 128 samples (2.7 ms) in all; the real chip's latency was not measured, and an emulator may trim it. `E` is normalised to 0 dB on average between 300 Hz and 3 kHz. **The real canceller is 5.5 dB (satellites) and 6.2 dB (monitors) below that (Strong).** Minerva's DirectSound 3D tests on a real card and in 86Box with headphones, with the same source and the same HRTF taps from the driver, give the real canceller's response for a centred source. Against the normalised model it is lower by those amounts, flat within about 1.5 dB from 200 Hz to 10 kHz (24 half-octave bands over two tests per setting). Run 2's tap fit (about −6.5 dB, satellites) agrees. The 86Box model applies them. Headphones are a straight pass-through.

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
