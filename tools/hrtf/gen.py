#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the headphone-surround filter table (xbox360/surround_tab.h) from a measured HRTF set.

    python3 -I tools/hrtf/gen.py --sofa /path/to/D1_48K_24bit_256tap_FIR_SOFA.sofa

Needs numpy, scipy and h5py (a venv outside the repo). The SOFA file is not in the repo: see
tools/hrtf/CREDITS.txt for where it comes from. Writes:
  xbox360/surround_tab.h      the table, 32 / 64 / 128 taps (XSR_TAPS picks one; 64 default)
  tools/hrtf/xsr_vectors.h    test vectors for tests/test_surround.c (from xsr_ref.py)
and prints a report (gains, delays, truncation error, loudness match).

Design (docs/ROADMAP.md 6.2): five speakers at ear level (0, +/-30, +/-110 degrees). The head is
treated as left/right symmetric, so each speaker pair is processed as sum and difference:
S = (L+R)/2 through Hi+Hc, D = (L-R)/2 through Hi-Hc, outL = S'+D', outR = S'-D', where Hi is
the near-ear (ipsilateral) response and Hc the far-ear one. Each response is rebuilt as
minimum phase from the measured magnitude plus a whole-sample delay for the far ear (the
interaural time difference), which is what lets 64 taps hold it: the measured set is
linear-phase, with pre-ringing that does not fit in 64 taps.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # xsr_ref (this directory only)

import h5py  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402
from scipy.spatial import SphericalVoronoi  # noqa: E402

import xsr_ref  # noqa: E402

RATE = 48000
NFFT = 4096                  # analysis grid: 11.7 Hz bins
AZ = {"front": 30, "surround": 110}
VARIANTS = (32, 64, 128)
SMOOTH_OCT = 1 / 6           # magnitude smoothing before the minimum-phase rebuild
DFE_LIMIT_DB = 12.0          # the diffuse-field correction is clipped to +/- this
LFE_DB = -6.0                # LFE in each ear, relative to a front channel in the plain mix
ITD_LP_HZ = 1500.0           # ITD is read below this (where it is heard)
NAMES = ("front-sum", "front-diff", "surround-sum", "surround-diff", "centre")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# ITU-R BS.1770 K-weighting at 48 kHz (the loudness match is measured through it)
K_SHELF = ([1.53512485958697, -2.69169618940638, 1.19839281085285],
           [1.0, -1.69065929318241, 0.73248077421585])
K_HPF = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])


def load_sofa(path):
    with h5py.File(path, "r") as f:
        conv = f.attrs.get("SOFAConventions", b"").decode()
        assert conv == "SimpleFreeFieldHRIR", conv
        units = f["SourcePosition"].attrs.get("Units", b"").decode()
        assert units.startswith("degree"), units
        ir = f["Data.IR"][:].astype(np.float64)
        pos = f["SourcePosition"][:].astype(np.float64)
        fs = int(round(float(f["Data.SamplingRate"][0])))
        assert not np.any(f["Data.Delay"][:]), "per-measurement delays are not handled"
    if fs != RATE:
        g = np.gcd(fs, RATE)
        ir = signal.resample_poly(ir, RATE // g, fs // g, axis=-1)
    return ir, pos, fs


def find(pos, az):
    """index of the ear-level measurement at azimuth az (SOFA: positive = to the left)"""
    d = np.abs((pos[:, 0] - az + 180) % 360 - 180)
    i = np.where((d < 0.01) & (np.abs(pos[:, 1]) < 0.01))[0]
    if len(i) != 1:
        raise SystemExit(f"azimuth {az}: {len(i)} measurements at ear level")
    return int(i[0])


def solid_angle_weights(pos):
    """Voronoi cell area of each direction, so dense parts of the grid don't dominate"""
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    v = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], axis=1)
    key = np.round(v, 6)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    sv = SphericalVoronoi(v[first], radius=1.0)
    area = sv.calculate_areas()
    counts = np.bincount(inv.reshape(-1))
    return area[inv.reshape(-1)] / counts[inv.reshape(-1)]


def smooth(p, frac):
    """fractional-octave smoothing of a power spectrum on the rfft grid"""
    n = len(p)
    f = np.arange(n, dtype=np.float64)
    c = np.concatenate([[0.0], np.cumsum(p)])
    lo = np.clip(np.floor(f * 2 ** (-frac / 2)), 0, n - 1).astype(int)
    hi = np.clip(np.ceil(f * 2 ** (frac / 2)), 1, n).astype(int)
    hi = np.maximum(hi, lo + 1)
    return (c[hi] - c[lo]) / (hi - lo)


def power(h):
    return np.abs(np.fft.rfft(h, NFFT)) ** 2


def minphase(mag, n):
    """minimum-phase impulse response (n taps) with magnitude mag (rfft grid of NFFT)"""
    full = np.concatenate([mag, mag[-2:0:-1]])
    cep = np.fft.ifft(np.log(np.maximum(full, 1e-8))).real
    fold = np.zeros(NFFT)
    fold[0] = cep[0]
    fold[1:NFFT // 2] = 2 * cep[1:NFFT // 2]
    fold[NFFT // 2] = cep[NFFT // 2]
    return np.fft.ifft(np.exp(np.fft.fft(fold))).real[:n]


def itd(l, r):
    """samples by which r lags l, read below ITD_LP_HZ, to 1/8 sample"""
    sos = signal.butter(4, ITD_LP_HZ, fs=RATE, output="sos")
    up = 8
    lu = signal.resample_poly(signal.sosfilt(sos, l), up, 1)
    ru = signal.resample_poly(signal.sosfilt(sos, r), up, 1)
    xc = signal.correlate(ru, lu, mode="full")
    return (np.argmax(xc) - (len(lu) - 1)) / up


def kweight_power(h, pink=True):
    """mean K-weighted power of white or pink noise through h (per unit input power)"""
    w = np.linspace(0, np.pi, NFFT // 2 + 1)
    _, k1 = signal.freqz(*K_SHELF, worN=w)
    _, k2 = signal.freqz(*K_HPF, worN=w)
    f = w / np.pi * RATE / 2
    band = (f >= 20) & (f <= 20000)
    sp = np.where(band, 1 / np.maximum(f, 1), 0) if pink else band.astype(float)
    hp = np.abs(np.fft.rfft(h, NFFT)) ** 2 if h is not None else 1.0
    return np.sum(sp * np.abs(k1 * k2) ** 2 * hp) / np.sum(sp * np.abs(k1 * k2) ** 2)


def thirds(h):
    """1/3-octave band levels (dB), 100 Hz .. 16 kHz"""
    p = power(h)
    f = np.arange(len(p)) * RATE / NFFT
    out = []
    for i in range(-10, 13):
        fc = 1000 * 2 ** (i / 3)
        b = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
        out.append(10 * np.log10(np.mean(p[b]) + 1e-20))
    return np.array(out)


def build_ears(ir, pos):
    """near / far ear responses (long, minimum phase, far ear delayed) per speaker group"""
    w = solid_angle_weights(pos)
    pdf = np.zeros(NFFT // 2 + 1)
    for e in (0, 1):
        pdf += np.einsum("m,mk->k", w, np.abs(np.fft.rfft(ir[:, e], NFFT, axis=-1)) ** 2)
    pdf /= 2 * w.sum()
    eq = 1 / np.sqrt(smooth(pdf, 1 / 3))
    f = np.arange(len(eq)) * RATE / NFFT
    eq /= np.sqrt(np.mean(eq[(f >= 200) & (f <= 2000)] ** 2))
    eq_db = np.clip(20 * np.log10(eq), -DFE_LIMIT_DB, DFE_LIMIT_DB)
    eq = 10 ** (eq_db / 20)
    rep = {"dfe_db": (eq_db[(f >= 100) & (f <= 16000)].min(), eq_db[(f >= 100) & (f <= 16000)].max())}

    def mag(*pairs):  # power average of mirrored measurements, smoothed, equalised
        p = sum(power(ir[i, e]) for i, e in pairs) / len(pairs)
        return np.sqrt(smooth(p, SMOOTH_OCT)) * eq

    ears = {}
    for name, az in AZ.items():
        il, ir_ = find(pos, az), find(pos, -az)  # left speaker, right speaker
        d = (itd(ir[il, 0], ir[il, 1]) + itd(ir[ir_, 1], ir[ir_, 0])) / 2
        near = minphase(mag((il, 0), (ir_, 1)), NFFT // 2)
        far = minphase(mag((il, 1), (ir_, 0)), NFFT // 2)
        # minimum phase gives the far (shadowed, duller) ear some low-frequency delay of its
        # own; the explicit delay adds only the rest, so the pair's ITD is the measured one
        own = itd(near, far)
        ears[name] = (near, far, d - own)
        rep[f"itd_{name}"] = (d, d - own)
    i0 = find(pos, 0)
    ears["centre"] = minphase(mag((i0, 0), (i0, 1)), NFFT // 2)
    rep["itd_centre"] = (itd(ir[i0, 0], ir[i0, 1]), 0)
    return ears, rep


def window(h, n):
    """the first n taps, the last quarter faded out (cos^2)"""
    w = np.ones(n)
    t = n // 4
    w[n - t:] = np.cos(np.linspace(0, np.pi / 2, t + 1)[1:]) ** 2
    return h[:n] * w


def far_delay(d, taps):
    """Whole-sample far-ear delay in a table of `taps`. A quarter of the taps is always left
    for the far ear's response, so a short table compresses a large ITD (32 taps: the
    surround pair's ~750 us becomes 500 us) instead of losing the far ear altogether."""
    return int(min(np.floor(d + 0.5), taps - taps // 4))


def pair(near, far, d, taps):
    """near and far ear filters of `taps`, each windowed over the taps it actually has"""
    dl = far_delay(d, taps)
    return window(near, taps), np.concatenate([np.zeros(dl), window(far, taps - dl)]), dl


def variant(ears, taps):
    """float filters (5, taps) incl. the loudness gain, and the report numbers"""
    fn, ff, dfr = pair(*ears["front"], taps)
    sn, sf, dsu = pair(*ears["surround"], taps)
    c = window(ears["centre"], taps)
    filt = np.stack([fn + ff, fn - ff, sn + sf, sn - sf, c])
    # loudness: FL and FR pink noise (uncorrelated), both ears' K-weighted power, ON vs OFF.
    # OFF puts FL into the left ear at K_FRONT; ON puts it into both through near / far.
    on = kweight_power(fn) + kweight_power(ff)
    off = (xsr_ref.K_FRONT / 16384) ** 2
    g = np.sqrt(off / on)
    rep = {"delays": (dfr, dsu), "trunc_loss_db": {}, "band_err_db": {}}
    for name, h, n in (("front near", ears["front"][0], taps), ("front far", ears["front"][1], taps - dfr),
                       ("surr near", ears["surround"][0], taps), ("surr far", ears["surround"][1], taps - dsu),
                       ("centre", ears["centre"], taps)):
        hw = window(h, n)
        rep["trunc_loss_db"][name] = 10 * np.log10(np.sum(hw ** 2) / np.sum(h ** 2))
        rep["band_err_db"][name] = np.max(np.abs(thirds(hw) - thirds(h)))
    return filt * g, rep


def quantise(filt, taps):
    """Q14 coefficients with sum|h| <= 2.0 (32768) per filter, and out_q14 / lfe_q14.
    The largest filter is scaled to the 2.0 limit (best precision); out_q14 undoes it."""
    peak = np.max(np.sum(np.abs(filt), axis=1))
    out_q14 = int(np.ceil(16384 * peak / 2.0))
    while True:
        h = np.round(filt * 16384 * 16384 / out_q14).astype(np.int64)
        if np.all(np.sum(np.abs(h), axis=1) <= 32768):
            break
        out_q14 += 1
    if out_q14 > 32767:
        raise SystemExit(f"{taps} taps: out_q14 {out_q14} does not fit (filters too loud)")
    lfe = xsr_ref.K_FRONT / 16384 * 10 ** (LFE_DB / 20)
    lfe_q14 = int(round(lfe * 16384 * 16384 / out_q14))
    if not 0 <= lfe_q14 <= 32767:
        raise SystemExit(f"{taps} taps: lfe_q14 {lfe_q14} does not fit")
    return h, out_q14, lfe_q14


def table_c(tabs, rep, src):
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/*",
        " * GENERATED by tools/hrtf/gen.py: do not edit. Change gen.py and regenerate.",
        " *",
        " * Headphone surround filters for xbox360/surround_dsp.h (struct xsr_table).",
        " * HRTF data: SADIE II database, subject D1 (Neumann KU100 dummy head), AudioLab,",
        " * University of York. Copyright 2018 University of York, Apache License 2.0.",
        " * https://www.york.ac.uk/sadie-project/database.html  doi:10.3390/app8112029",
        " * Derived (modified) data: see tools/hrtf/CREDITS.txt for what was changed.",
        f" * Source file: {os.path.basename(src)}; speakers at 0, +/-30, +/-110 degrees, ear level.",
        f" * Measured ITD: front {rep['itd_front'][0]:.1f}, surround {rep['itd_surround'][0]:.1f} samples"
        " (48 kHz); the far-ear delay in each table is below.",
        " * Rows: front-sum, front-diff, surround-sum, surround-diff, centre (Q14, sum|h| <= 2.0).",
        " */",
        "#pragma once",
        "",
    ]
    first = True
    for taps in VARIANTS:
        h, outq, lfeq, (dfr, dsu) = tabs[taps]
        lines.append(f"#{'if' if first else 'elif'} XSR_TAPS == {taps}")
        lines.append(f"/* far-ear delay: front {dfr}, surround {dsu} samples */")
        first = False
        lines += [
            "static const struct xsr_table xsr_tab = {",
            "\t.magic = XSR_MAGIC,",
            "\t.version = XSR_VERSION,",
            f"\t.taps = {taps},",
            f"\t.rate = {RATE},",
            f"\t.lfe_q14 = {lfeq},",
            f"\t.out_q14 = {outq},",
            f"\t.crc = 0x{xsr_ref.table_crc(h):08x},",
            "\t.h = {",
        ]
        for f in range(5):
            vals = [str(int(v)) for v in h[f]]
            rows = [", ".join(vals[i:i + 12]) for i in range(0, len(vals), 12)]
            lines.append(f"\t\t/* {NAMES[f]} */")
            lines.append("\t\t{ " + (",\n\t\t  ".join(rows)) + " },")
        lines += ["\t},", "};"]
    lines += ['#else', '#error "surround_tab.h: XSR_TAPS must be 32, 64 or 128"', "#endif", ""]
    return "\n".join(lines)


# ---- test vectors (tests/test_surround.c) ---------------------------------------------------
VEC_N = 868                   # blocks of 256, 255, 101, 256 in the C test
VEC_SEED = 0x12345678
MEAS_N = 48000
MEAS_SEED = 0xACE1
FS_SEED = 0x1000              # full-scale noise: channel k uses FS_SEED + k
HEADROOM_DB = 0.5            # full-scale 5-channel noise must peak this far under full scale


def vec_input():
    x = np.zeros((6, VEC_N), dtype=np.int64)
    for ch in range(6):
        x[ch] = xsr_ref.noise16((VEC_SEED + ch * 0x9E3779B9) & 0xFFFFFFFF, VEC_N)
        x[ch, :32] = -32768 if ch % 2 == 0 else 32767   # the extremes, for the overflow bound
    return x


def lowpass_coefs():
    b, a = signal.butter(2, ITD_LP_HZ, fs=RATE)
    return b, a


def measure(out):
    """ILD (dB, left over right) and ITD (samples, right later) as the C test reads them"""
    l, r = out.astype(np.float64)
    ild = 10 * np.log10(np.sum(l * l) / np.sum(r * r))
    b, a = lowpass_coefs()
    lf, rf = signal.lfilter(b, a, l), signal.lfilter(b, a, r)
    lags = range(-48, 49)
    xc = [np.dot(lf[48:-48], rf[48 + k:len(rf) - 48 + k]) for k in lags]
    return ild, list(lags)[int(np.argmax(xc))]


def one_channel(ch):
    x = np.zeros((6, MEAS_N), dtype=np.int64)
    x[ch] = xsr_ref.noise16(MEAS_SEED, MEAS_N) >> 2
    return x


def vectors_c(tabs_ref):
    b, a = lowpass_coefs()
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-or-later */",
        "/* GENERATED by tools/hrtf/gen.py (from xsr_ref.py): do not edit. tests/test_surround.c */",
        "#pragma once",
        f"#define XV_N {VEC_N}",
        f"#define XV_SEED 0x{VEC_SEED:08x}u",
        f"#define XV_MEAS_N {MEAS_N}",
        f"#define XV_MEAS_SEED 0x{MEAS_SEED:x}u",
        f"#define XV_FS_SEED 0x{FS_SEED:x}u",
        f"static const double xv_lp_b[3] = {{ {float(b[0])!r}, {float(b[1])!r}, {float(b[2])!r} }};",
        f"static const double xv_lp_a[3] = {{ {float(a[0])!r}, {float(a[1])!r}, {float(a[2])!r} }};",
    ]
    first = True
    x = vec_input()
    for taps in VARIANTS:
        tab = tabs_ref[taps]
        out, clamps = xsr_ref.process(tab, x)
        inter = xsr_ref.interleave(out)
        xf = xsr_ref.interleave(xsr_ref.crossfade(xsr_ref.downmix(x[:, :256]), out[:, :256]))
        ild_f, itd_f = measure(xsr_ref.process(tab, one_channel(xsr_ref.FL))[0])
        ild_s, itd_s = measure(xsr_ref.process(tab, one_channel(xsr_ref.SL))[0])
        lines += [
            f"#{'if' if first else 'elif'} XSR_TAPS == {taps}",
            f"#define XV_OUT_CRC 0x{xsr_ref.crc16s(inter):08x}u",
            f"#define XV_CLAMPS {clamps}",
            f"#define XV_XFADE_CRC 0x{xsr_ref.crc16s(xf):08x}u",
            f"#define XV_FL_ILD {float(ild_f)!r}",
            f"#define XV_FL_ITD {itd_f}",
            f"#define XV_SL_ILD {float(ild_s)!r}",
            f"#define XV_SL_ITD {itd_s}",
            f"#define XV_ONOFF_DB {float(loudness_on_off(tab)):.3f}",
            f"#define XV_FS_CLAMPS {xsr_ref.process(tab, full_scale_noise())[1]}",
            "static const s16 xv_out_head[64] = { " + ", ".join(str(int(v)) for v in inter[:64]) + " };",
        ]
        first = False
    lines += ["#endif", ""]
    return "\n".join(lines)


def full_scale_noise():
    """full-scale noise on the 5 main channels (LFE silent): the clipping check"""
    x = np.zeros((6, MEAS_N), dtype=np.int64)
    for k, ch in enumerate((xsr_ref.FL, xsr_ref.FR, xsr_ref.C, xsr_ref.SL, xsr_ref.SR)):
        x[ch] = xsr_ref.noise16(FS_SEED + k, MEAS_N)
    return x


def headroom(h, outq, lfeq, taps):
    """--headroom: lower out_q14 until full-scale 5-channel noise peaks HEADROOM_DB under
    full scale. The default (loudness-matched) table peaks about +3.6 dBFS on that input:
    virtual speakers put every channel into both ears, through ear resonances the plain mix
    does not have. Mastered game audio does not reach it: on the demo scene and on
    correlated bursts on all five channels pushed until the plain mix peaks at -0.1 dBFS,
    the matched table peaks at -0.1 to -1.3 dBFS and never clamps (the kernel clamps and
    counts if it ever does). Off by default, because a 4 dB drop when the player switches
    surround on reads as "worse"."""
    x = full_scale_noise()
    limit = 32767 * 10 ** (-HEADROOM_DB / 20)
    start = outq
    while True:
        tab = dict(h=h, out_q14=outq, lfe_q14=lfeq)
        pk = np.max(np.abs(xsr_ref.process(tab, x, raw=True)[0]))
        if pk <= limit:
            return outq, 20 * np.log10(outq / start)
        outq = int(outq * limit / pk)


def pink_pair(n, seed=1):
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal((2, n)), axis=1)
    pink = np.fft.irfft(spec / np.sqrt(np.maximum(np.arange(n // 2 + 1), 1)), n, axis=1)
    return np.round(pink / np.max(np.abs(pink)) * 8000).astype(np.int64)


def loudness_on_off(tab):
    """ON minus OFF (dB): uncorrelated pink noise on FL and FR, K-weighted, both ears"""
    n = 1 << 18
    x = np.zeros((6, n), dtype=np.int64)
    x[xsr_ref.FL], x[xsr_ref.FR] = pink_pair(n)
    on, _ = xsr_ref.process(tab, x)
    off = xsr_ref.downmix(x)

    def kw(s):
        s = signal.lfilter(*K_SHELF, s.astype(np.float64))
        return np.mean(signal.lfilter(*K_HPF, s)[4096:] ** 2)
    return 10 * np.log10((kw(on[0]) + kw(on[1])) / (kw(off[0]) + kw(off[1])))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sofa", required=True, help="SADIE II D1 48 kHz HRIR SOFA file")
    ap.add_argument("--tab", default=os.path.join(ROOT, "xbox360", "surround_tab.h"))
    ap.add_argument("--vec", default=os.path.join(HERE, "xsr_vectors.h"))
    ap.add_argument("--headroom", action="store_true",
                    help="lower the gain (about 4 dB) until full-scale noise on all five main "
                         "channels cannot clip, instead of matching the plain mix's loudness")
    args = ap.parse_args()

    ir, pos, fs = load_sofa(args.sofa)
    ears, rep = build_ears(ir, pos)
    print(f"source: {os.path.basename(args.sofa)}, {fs} Hz, {ir.shape[0]} directions")
    print(f"diffuse-field correction applied: {rep['dfe_db'][0]:+.1f} .. {rep['dfe_db'][1]:+.1f} dB"
          " (100 Hz-16 kHz; the set is already diffuse-field equalised)")
    for k in ("front", "surround", "centre"):
        d, dl = rep[f"itd_{k}"]
        print(f"ITD {k}: {d:.2f} samples measured ({d / RATE * 1e6:.0f} us); far-ear delay "
              f"after the minimum-phase rebuild {dl:.2f}")

    tabs = {}
    for taps in VARIANTS:
        filt, vrep = variant(ears, taps)
        h, outq, lfeq = quantise(filt, taps)
        cut = 0.0
        if args.headroom:
            outq, cut = headroom(h, outq, lfeq, taps)
        tabs[taps] = (h, outq, lfeq, vrep["delays"])
        sums = np.sum(np.abs(h), axis=1)
        print(f"\n{taps} taps: out_q14 {outq} ({outq / 16384:.3f}, {cut:+.2f} dB for headroom), "
              f"lfe_q14 {lfeq}, crc 0x{xsr_ref.table_crc(h):08x}, table {20 + 5 * taps * 2} bytes")
        print(f"  far-ear delay used: front {vrep['delays'][0]}, surround {vrep['delays'][1]} samples")
        print("  sum|h| per filter (Q14, limit 32768): " + ", ".join(str(int(s)) for s in sums))
        print("  truncation energy loss (dB): " +
              ", ".join(f"{k} {v:.2f}" for k, v in vrep["trunc_loss_db"].items()))
        print("  worst 1/3-octave error vs 2048-tap (dB): " +
              ", ".join(f"{k} {v:.1f}" for k, v in vrep["band_err_db"].items()))

    with open(args.tab, "w") as f:
        f.write(table_c(tabs, rep, args.sofa))
    ref = xsr_ref.parse_tab(args.tab)  # everything below runs on the committed numbers

    print("\nchecks on the quantised table (reference model):")
    for taps in VARIANTS:
        tab = ref[taps]
        raw, cl = xsr_ref.process(tab, full_scale_noise(), raw=True)
        pk = np.max(np.abs(raw))
        ild_f, itd_f = measure(xsr_ref.process(tab, one_channel(xsr_ref.FL))[0])
        ild_s, itd_s = measure(xsr_ref.process(tab, one_channel(xsr_ref.SL))[0])
        print(f"  {taps:3d} taps: loudness ON-OFF {loudness_on_off(tab):+.2f} dB (front pink, "
              f"K-weighted); full-scale 5-ch noise: clamps {cl}, peak {20 * np.log10(pk / 32767):+.2f}"
              f" dBFS; FL ILD {ild_f:.1f} dB ITD {itd_f}; SL ILD {ild_s:.1f} dB ITD {itd_s}")

    with open(args.vec, "w") as f:
        f.write(vectors_c(ref))
    print(f"\nwrote {os.path.relpath(args.tab, ROOT)} and {os.path.relpath(args.vec, ROOT)}")


if __name__ == "__main__":
    main()
