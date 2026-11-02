"""Aureal Vortex 2 crosstalk canceller - empirical reference model.

Measured on a real SQ2500 (driver 4.06.2048) through the line-in: the canceller's response
for the control panel's two speaker settings, fitted over 7 click positions per mode and
validated on an independent session (44 positions; error 4.5-10% below 2.4 kHz).

    out_L = E * (delay(in_L) + X * in_R)
    out_R = E * (delay(in_R) + X * in_L)

E (per-ear EQ) and X (crosstalk) are 512-tap FIRs at 48 kHz carrying a 64-sample bulk
latency each. Headphone mode is a straight pass-through. The register-level arithmetic of
the canceller is not decoded; this model reproduces what the driver's three presets do.
"""
import numpy as np, os

LATENCY = 64
_here = os.path.dirname(os.path.abspath(__file__))
_fir = np.load(os.path.join(_here, 'vortex_xtalk_fir.npz'))

def process(in_l, in_r, mode='wide'):
    """mode: 'wide' (satellite speakers, the default), 'narrow' (monitors) or 'headphones'."""
    in_l = np.asarray(in_l, float); in_r = np.asarray(in_r, float)
    if mode == 'headphones':
        return in_l.copy(), in_r.copy()
    E, X = _fir[f'{mode}_E'], _fir[f'{mode}_X']
    n = len(in_l)
    d = lambda x: np.concatenate([np.zeros(LATENCY), x])[:n]
    c = lambda x, h: np.convolve(x, h)[:n]
    out_l = c(d(in_l) + c(in_r, X), E)
    out_r = c(d(in_r) + c(in_l, X), E)
    return out_l, out_r
