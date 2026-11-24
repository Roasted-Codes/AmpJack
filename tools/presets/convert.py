#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds xw.presets (Python standard library only): tools/presets/xw.presets (committed) and
a copy in xbox360/out/ for eq.py install.

    python3 tools/presets/convert.py            # tools/presets/xw.presets + xbox360/out copy
    python3 tools/presets/convert.py --vectors  # also dist/eq_vectors.bin for tests/test_eq.c

Steps: read names.txt (what ships, panel order, short names, descriptions) and each preset's
file: TheZEFERENCE's .astroeq (upstream/) or this project's own .xweq (xw/, format in
xw/bass-boost.xweq); design five biquads per preset at 48 kHz in floating point, quantise them to
the kernel's Q3.28, pick a Q15 preamp so the quantised curve's peak is <= 0 dB and every preset
and FLAT are equally loud (LOUDNESS below), prove no band output can leave s32 (L1 bound),
build the CUSTOM tables (custom.py), write the big-endian file (eq_360.h, version 2), read it
back through eq_model.py and compare. Any failed check stops the build with a message;
nothing is written then.

The .astroeq format (Astro Command Center, INI): [General] Name; [Eq_Bands] Eq_Band_1..5 gains,
Eq_Freq_Band_1..5 centre frequencies, Eq_Bandwidth_2..4. What is known about the numbers
(tdryer/eh-fifty, which drives the A50 base station's EQ over USB):
  - gains are whole dB, -7..+7 (the device takes dB + 12);
  - frequencies are Hz, 80..15000;
  - bandwidth exists for bands 2-4 only, 0.1..3.0, sent as value x 4096; the device refuses
    any bandwidth for bands 1 and 5, and Command Center labels it "xCF" (a multiple of the
    centre frequency).
Not documented anywhere found: the filter shapes. A band with a gain and a frequency but no
width is a shelf in every 5-band EQ of this kind, so bands 1 and 5 are read as shelves; the
shelf slope, where the shelf frequency sits on the curve, and how "xCF" maps to Q are
assumptions. They all live in BAND_SEMANTICS below; change that one name to try another
reading. Astro's own preamp / headroom handling is also unknown; this tool adds its own.
"""
import argparse
import cmath
import configparser
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eq_model  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
UPSTREAM = os.path.join(HERE, "upstream")
OWN = os.path.join(HERE, "xw")
NAMES = os.path.join(HERE, "names.txt")
DIST = os.path.join(ROOT, "dist")

FS = 48000
QBITS = 28  # Q3.28: |c| < 8
AUTHOR = "ZaliaS"  # every upstream preset; an .xweq names its own
# A description is what the panel's one help line says while the cursor is on PRESET: the line
# holds 36 characters (xbox360/hud_panel.h HP_HELP_COLS). The file's field stays 64 bytes
# (eq_model.DESC, eq_360.h XQ_DESC_LEN: no format change); a build that meets a longer text
# from an older file cuts it at a word, so this check is what keeps every word on screen.
HELP_COLS = 36
DESC_MAX = min(HELP_COLS, eq_model.DESC - 1)
MAX_PRESETS = 64  # XQ_MAX_PRESETS
# Characters the panel font can draw (xbox360/hud_panel.c FONT_CHARS) plus space.
GLYPHS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-%+:/.<> ")

# ---- the one switch for how the Astro numbers are read -------------------------------------
#   "xcf"    bands 1 / 5: RBJ cookbook low / high shelf, slope S = 1, frequency = the shelf's
#            half-gain point; bands 2-4: RBJ peaking, Q = 1 / bandwidth (bandwidth in Hz =
#            bandwidth x centre frequency between the half-gain points, Command Center's "xCF").
#   "octave" the same shelves; bandwidth read as RBJ octaves between the half-gain points.
#   "peaks"  all five bands RBJ peaking; bands 1 / 5 at Q = 1/sqrt(2); bands 2-4 as "xcf".
BAND_SEMANTICS = "xcf"
SEMANTICS = ("xcf", "octave", "peaks")

# ---- how loud a preset plays ---------------------------------------------------------------
#   "match"  every preset and FLAT play equally loud on pink noise, K-weighted as in ITU-R
#            BS.1770 (how loudness meters hear): each preset's preamp is lowered from its
#            peak-safe value to the level of the quietest preset, and FLAT is turned down to
#            that same level (the header's flat_q15) while a preset file is loaded. Switching
#            presets then never jumps in loudness, no preset can clip (peaks stay <= 0 dB, so
#            no limiter is needed and the no-wrap proof holds), and the whole headset is a few
#            dB quieter than with no file, which the VOLUME row (the headset's own amplifier)
#            buys back.
#   "peak"   each preset at its peak-safe preamp, FLAT untouched: presets are a few dB
#            quieter than FLAT, and the panel jumps in loudness when switching.
# A preset whose file says Level = own (a band-pass; none ships now) is left out of "the quietest
# preset": a band-pass removes most of the sound, and matching it would take every other
# preset and FLAT down by its loss. It is matched downward when it can be, and otherwise
# plays at its peak-safe preamp, quieter than the rest (convert.py prints by how much).
# CUSTOM is left out the same way (custom.py).
LOUDNESS = "match"
LOUDNESS_MODES = ("match", "peak")
# ITU-R BS.1770-4 K-weighting at 48 kHz (its published coefficients): a +4 dB shelf above
# about 1.5 kHz, then a high-pass at about 38 Hz.
K_WEIGHT = [[1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
             0.73248077421585],
            [1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621]]


def fail(msg):
    sys.exit("convert.py: " + msg)


# ---- reading -----------------------------------------------------------------------------

def read_names():
    """(shipped, dropped): shipped = [(short, path relative to tools/presets, description)],
    dropped = [path]"""
    ship, dropped = [], []
    with open(NAMES, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if len(parts) != 3:
                fail("names.txt line %d: SHORT | file | DESCRIPTION" % n)
            if parts[0] == "DROPPED":
                dropped.append(parts[1])
            else:
                ship.append(tuple(parts))
    return ship, dropped


def read_astroeq(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8-sig")
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read_string(text)
    e = ini["Eq_Bands"]
    try:
        gains = [float(e["Eq_Band_%d" % i]) for i in range(1, 6)]
        freqs = [float(e["Eq_Freq_Band_%d" % i]) for i in range(1, 6)]
        bws = [None] + [float(e["Eq_Bandwidth_%d" % i]) for i in range(2, 5)] + [None]
    except KeyError as k:
        fail("%s: missing %s" % (path, k))
    for g in gains:
        if not -7 <= g <= 7:
            fail("%s: gain %g outside -7..7 dB" % (path, g))
    for fr in freqs:
        if not 20 <= fr <= 20000:
            fail("%s: frequency %g Hz" % (path, fr))
    for bw in bws[1:4]:
        if not 0.1 <= bw <= 3.0:
            fail("%s: bandwidth %g outside 0.1..3.0" % (path, bw))
    return dict(name=ini["General"].get("Name", ""), gains=gains, freqs=freqs, bws=bws)


LEVELS = ("match", "own")
TYPES = ("lowshelf", "highshelf", "peak", "highpass", "lowpass")


def read_xweq(path):
    """this project's own preset (format: xw/bass-boost.xweq): dict(author, level, bands), bands =
    [(type, f0, gain, q or slope)]"""
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    with open(path, encoding="utf-8") as f:
        ini.read_string(f.read())
    g = ini["General"]
    author, level = g.get("Author", ""), g.get("Level", "match")
    if not author or len(author) > eq_model.AUTHOR or not author.isascii():
        fail("%s: Author: 1..16 ASCII characters" % path)
    if level not in LEVELS:
        fail("%s: Level: one of %s" % (path, ", ".join(LEVELS)))
    bands = []
    for i in range(1, 6):
        if "Band_%d" % i not in ini:
            continue
        b = ini["Band_%d" % i]
        kind = b.get("Type", "")
        if kind not in TYPES:
            fail("%s: Band_%d Type: one of %s" % (path, i, ", ".join(TYPES)))
        shelfy = "shelf" in kind
        try:
            f0 = float(b["Freq"])
            gain = float(b.get("Gain", "0"))
            q = float(b.get("Slope", "1")) if shelfy else float(b.get("Q", "0.707"))
        except (KeyError, ValueError) as e:
            fail("%s: Band_%d: %s" % (path, i, e))
        if not 10 <= f0 <= 23000 or not -24 <= gain <= 24 or not 0.1 <= q <= 20 or \
                (shelfy and q > 1):
            fail("%s: Band_%d: Freq 10..23000, Gain -24..24, Q 0.1..20, Slope 0.1..1" % (path, i))
        bands.append((kind, f0, 0.0 if "pass" in kind else gain, q))
    if len(ini.sections()) - 1 != len(bands):
        fail("%s: sections other than General and Band_1..Band_5" % path)
    return dict(author=author, level=level, bands=bands)


# ---- design (floating point, Mac only) ---------------------------------------------------

def peaking(f0, g, q):
    a = 10 ** (g / 40)
    w = 2 * math.pi * f0 / FS
    al = math.sin(w) / (2 * q)
    cw = math.cos(w)
    a0 = 1 + al / a
    return [(1 + al * a) / a0, -2 * cw / a0, (1 - al * a) / a0, -2 * cw / a0, (1 - al / a) / a0]


def peaking_oct(f0, g, bw):
    w = 2 * math.pi * f0 / FS
    q = 1 / (2 * math.sinh(math.log(2) / 2 * bw * w / math.sin(w)))
    return peaking(f0, g, q)


def shelf(f0, g, high, slope=1.0):
    a = 10 ** (g / 40)
    w = 2 * math.pi * f0 / FS
    cw, sw = math.cos(w), math.sin(w)
    al = sw / 2 * math.sqrt((a + 1 / a) * (1 / slope - 1) + 2)
    r = 2 * math.sqrt(a) * al
    s = -1 if high else 1  # the high shelf is the low shelf with cos(w) -> -cos(w) and b1, a1 negated
    b0 = a * ((a + 1) - s * (a - 1) * cw + r)
    b1 = s * 2 * a * ((a - 1) - s * (a + 1) * cw)
    b2 = a * ((a + 1) - s * (a - 1) * cw - r)
    a0 = (a + 1) + s * (a - 1) * cw + r
    a1 = -s * 2 * ((a - 1) + s * (a + 1) * cw)
    a2 = (a + 1) + s * (a - 1) * cw - r
    return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0]


def passband(f0, q, high):
    """RBJ cookbook 2nd-order high-pass (high) or low-pass; Q 0.707 is Butterworth"""
    w = 2 * math.pi * f0 / FS
    al = math.sin(w) / (2 * q)
    cw = math.cos(w)
    a0 = 1 + al
    if high:
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
    else:
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
    return [b[0] / a0, b[1] / a0, b[2] / a0, -2 * cw / a0, (1 - al) / a0]


def section(kind, f0, gain, q):
    """one biquad of an .xweq (and of custom.py): q is the slope for a shelf"""
    if kind == "lowshelf":
        return shelf(f0, gain, high=False, slope=q)
    if kind == "highshelf":
        return shelf(f0, gain, high=True, slope=q)
    if kind == "peak":
        return peaking(f0, gain, q)
    return passband(f0, q, high=(kind == "highpass"))


def design(p, semantics):
    if "bands" in p:  # an .xweq: its sections, then unity
        out = [section(*b) for b in p["bands"]]
        return out + [[1.0, 0.0, 0.0, 0.0, 0.0]] * (5 - len(out))
    out = []
    for k in range(5):
        g, f0, bw = p["gains"][k], p["freqs"][k], p["bws"][k]
        if g == 0:
            out.append([1.0, 0.0, 0.0, 0.0, 0.0])
        elif k in (0, 4) and semantics != "peaks":
            out.append(shelf(f0, g, high=(k == 4)))
        elif k in (0, 4):
            out.append(peaking(f0, g, 1 / math.sqrt(2)))
        elif semantics == "octave":
            out.append(peaking_oct(f0, g, bw))
        else:
            out.append(peaking(f0, g, 1 / bw))
    return out


def response(coefs, f, scale=1.0):
    """|H| of a cascade at f Hz; coefs as floats (b0 b1 b2 a1 a2)."""
    z1 = cmath.exp(-2j * math.pi * f / FS)
    z2 = z1 * z1
    h = complex(scale)
    for b0, b1, b2, a1, a2 in coefs:
        h *= (b0 + b1 * z1 + b2 * z2) / (1 + a1 * z1 + a2 * z2)
    return abs(h)


def db(x):
    return 20 * math.log10(max(x, 1e-12))  # a high-pass is exactly 0 at 0 Hz


def pink_db(coefs):
    """Loudness of a cascade on pink noise relative to no filter, K-weighted: the mean of
    |H K|^2 over log-spaced frequencies 10 Hz .. 24 kHz (pink noise has equal power per
    octave, so an even log grid weights it right; the whole 48 kHz band, because the
    earpiece mix has content up to 24 kHz), against the same for K alone."""
    grid = [10 * 2399 ** (i / 2399) for i in range(2400)]
    num = sum((response(coefs, f) * response(K_WEIGHT, f)) ** 2 for f in grid)
    den = sum(response(K_WEIGHT, f) ** 2 for f in grid)
    return 10 * math.log10(num / den)


def peak_db(coefs):
    """Maximum of |H| in dB over 0..FS/2: a dense log grid, then golden-section refinement
    around every local maximum (the curves are smooth, so this finds the true peak)."""
    grid = [0.0] + [5 * (FS / 2 / 5) ** (i / 3999) for i in range(4000)]
    vals = [db(response(coefs, f)) for f in grid]
    best = max(vals)
    for i in range(1, len(grid) - 1):
        if vals[i] >= vals[i - 1] and vals[i] >= vals[i + 1]:
            lo, hi = grid[i - 1], grid[i + 1]
            for _ in range(60):
                m1, m2 = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
                if db(response(coefs, m1)) < db(response(coefs, m2)):
                    lo = m1
                else:
                    hi = m2
            best = max(best, db(response(coefs, (lo + hi) / 2)))
    return max(best, vals[-1])


# ---- quantising and the range proofs ------------------------------------------------------

def quantise(coefs):
    q = []
    for band in coefs:
        row = [int(round(c * (1 << QBITS))) for c in band]
        for v in row:
            if not -(1 << 31) <= v < (1 << 31):
                fail("coefficient %r does not fit Q3.28" % v)
        q.append(row)
    return q


def l1_bounds(qc, active, x_max):
    """Runs an impulse through the quantised bands in order; for each active band returns
    (bound on |band output|, bound on |accumulator|) in the kernel's integer units."""
    n = 16384
    h = [1.0] + [0.0] * (n - 1)
    x_bound = float(x_max)
    out = []
    for k in range(5):
        if not active & (1 << k):
            continue
        b0, b1, b2, a1, a2 = (c / (1 << QBITS) for c in qc[k])
        x1 = x2 = y1 = y2 = 0.0
        y = [0.0] * n
        for i in range(n):
            v = b0 * h[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            y[i] = v
            x2, x1, y2, y1 = x1, h[i], y1, v
        h = y
        peak = max(abs(v) for v in h)
        if max(abs(v) for v in h[-1024:]) > 1e-9 * peak:
            fail("impulse response has not decayed in %d samples" % n)
        l1 = sum(abs(v) for v in h)
        # +1 % and +1024 LSB cover the rounding of every band before this one.
        y_bound = l1 * x_max * 1.01 + 1024
        acc = (sum(abs(c) for c in qc[k][:3]) * x_bound + sum(abs(c) for c in qc[k][3:]) * y_bound
               + (1 << 27))
        out.append((l1, y_bound, acc))
        x_bound = y_bound
    return out


def build(semantics, loudness):
    names, dropped = read_names()
    shorts, ids = set(), set()
    presets = []
    if not 0 < len(names) <= MAX_PRESETS:
        fail("%d presets: 1..%d" % (len(names), MAX_PRESETS))
    for short, rel, desc in names:
        if len(short) > eq_model.NAME or not set(short) <= GLYPHS or short != short.strip() \
                or not short:
            fail("short name %r: at most 20 panel-font characters" % short)
        if short in ("FLAT", "CUSTOM"):
            fail("%s is the console's own entry" % short)
        if not desc or len(desc) > DESC_MAX or not set(desc) <= GLYPHS:
            fail("%s: description %r (%d characters): 1..%d panel-font characters, one help "
                 "line (upper case, no comma or apostrophe)" % (short, desc, len(desc), DESC_MAX))
        pid = eq_model.fnv1a(short)
        if short in shorts or pid in ids or pid == 0:
            fail("short name %r: duplicate name or id" % short)
        shorts.add(short)
        ids.add(pid)
        path = os.path.join(HERE, rel)
        if not os.path.exists(path):
            fail("missing %s" % rel)
        if rel.startswith("upstream/") and rel.endswith(".astroeq"):
            p = read_astroeq(path)
            p.update(author=AUTHOR, level="match")
        elif rel.startswith("xw/") and rel.endswith(".xweq"):
            p = read_xweq(path)
            p["gains"] = [b[2] for b in p["bands"]] + [0.0] * (5 - len(p["bands"]))
            # five, as an .astroeq has (the test vectors check every preset at as many points)
            p["freqs"] = [b[1] for b in p["bands"]] + [1000.0] * (5 - len(p["bands"]))
        else:
            fail("%s: an upstream/*.astroeq or xw/*.xweq file" % rel)
        p.update(short=short, rel=rel, id=pid, desc=desc)
        presets.append(p)
    listed = {rel for _, rel, _ in names}
    for rel in dropped:
        if rel in listed or not os.path.exists(os.path.join(HERE, rel)):
            fail("DROPPED %s: listed as shipped too, or no such file" % rel)
    for top, ext in ((UPSTREAM, ".astroeq"), (OWN, ".xweq")):
        for d, _, files in os.walk(top):
            for fn in files:
                rel = os.path.relpath(os.path.join(d, fn), HERE)
                if fn.endswith(ext) and rel not in listed and rel not in dropped:
                    fail("%s is neither shipped nor DROPPED in names.txt" % rel)

    for p in presets:
        fc = design(p, semantics)
        qc = quantise(fc)
        qf = [[c / (1 << QBITS) for c in row] for row in qc]
        pk = peak_db(qf)
        safe = 32768 if pk <= 0 else int(math.floor(32768 * 10 ** (-pk / 20)))
        p.update(fc=fc, qc=qc, qf=qf, peak=pk, safe=safe, pink=pink_db(qf))
    # the common level: the quietest preset at its peak-safe preamp (never above 0 dB)
    if loudness == "match":
        target = min(0.0, min(p["pink"] + db(p["safe"] / 32768) for p in presets
                              if p["level"] == "match"))
        flat_q15 = int(round(32768 * 10 ** (target / 20)))
    else:
        flat_q15 = 32768

    for p in presets:
        fc, qc, qf, pk = p["fc"], p["qc"], p["qf"], p["peak"]
        active = sum(1 << k for k in range(5) if qc[k] != [1 << QBITS, 0, 0, 0, 0])
        if loudness == "match":
            pre = int(round(32768 * 10 ** ((db(flat_q15 / 32768) - p["pink"]) / 20)))
            pre = min(pre, p["safe"])
        else:
            pre = p["safe"]
        after = pk + db(pre / 32768)
        loud = p["pink"] + db(pre / 32768)
        if loudness == "match" and p["level"] == "match" and \
                abs(loud - db(flat_q15 / 32768)) > 0.01:
            fail("%s: loudness %+.3f dB, FLAT %+.3f dB" % (p["short"], loud,
                                                           db(flat_q15 / 32768)))
        if after > 1e-9:
            fail("%s: peak after preamp %+.4f dB" % (p["short"], after))
        x_max = (32768 * pre + 64) >> 7
        bounds = l1_bounds(qc, active, x_max)
        for l1, yb, acc in bounds:
            if yb >= 2 ** 31 - 1 or acc >= 2 ** 63 - 1:
                fail("%s: a band output or accumulator can overflow" % p["short"])
        # quantisation error of the curve, 10 Hz .. 24 kHz
        qerr = max(abs(db(response(qf, f)) - db(response(fc, f)))
                   for f in (10 * 2400 ** (i / 499) for i in range(500)))
        if loud > db(flat_q15 / 32768) + 0.01:
            fail("%s: louder than FLAT" % p["short"])
        flags = active | (eq_model.F_OWN_LEVEL if p["level"] == "own" else 0)
        p.update(active=active, flags=flags, pre=pre, after=after, loud=loud,
                 max_y=max(b[1] for b in bounds) if bounds else 0,
                 max_acc=max(b[2] for b in bounds) if bounds else 0, qerr=qerr)
    return presets, flat_q15


def pack(presets, flat_q15, ctab):
    import custom  # here, not at the top: custom.py imports this module
    recs = []
    for p in presets:
        r = struct.pack(">I", p["id"])
        r += p["short"].encode("ascii").ljust(eq_model.NAME, b"\0")
        r += p["author"].encode("ascii").ljust(eq_model.AUTHOR, b"\0")
        r += struct.pack(">iI", p["pre"], p["flags"])
        r += struct.pack(">25i", *[v for row in p["qc"] for v in row])
        r += struct.pack(">5h", *[int(round(g * 100)) for g in p["gains"]])
        r += b"\0\0"
        r += p["desc"].encode("ascii").ljust(eq_model.DESC, b"\0")
        assert len(r) == eq_model.REC
        recs.append(r)
    body = b"".join(recs) + custom.pack(ctab)
    hdr = struct.pack(">4s7I", eq_model.MAGIC, eq_model.VERSION, FS, len(presets), eq_model.REC,
                      5, zlib.crc32(body), flat_q15)
    return hdr + body


def roundtrip(data, presets, flat_q15, ctab):
    back = eq_model.read_presets(data)
    cback, err = eq_model.read_custom(data, flat_q15)
    if err or cback["c"] != ctab["c"] or cback["pre"] != ctab["pre"]:
        fail("round trip: the CUSTOM tables (%d)" % err)
    if eq_model.read_flat_q15(data) != flat_q15:
        fail("round trip: flat_q15")
    if len(back) != len(presets):
        fail("round trip: count")
    for p, b in zip(presets, back):
        if (b["id"] != p["id"] or b["name"] != p["short"] or b["author"] != p["author"]
                or b["bands"].c != p["qc"] or b["bands"].preamp_q15 != p["pre"]
                or b["bands"].active != p["active"] or b["desc"] != p["desc"]
                or b["flags"] != p["flags"]
                or b["gain_cdb"] != [int(round(g * 100)) for g in p["gains"]]):
            fail("round trip: record %s differs" % p["short"])
    return back


def shown(path):
    path = os.path.abspath(path)
    return os.path.relpath(path, ROOT) if path.startswith(ROOT + os.sep) else path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--semantics", choices=SEMANTICS, default=BAND_SEMANTICS)
    ap.add_argument("--loudness", choices=LOUDNESS_MODES, default=LOUDNESS)
    ap.add_argument("--out", default=os.path.join(HERE, "xw.presets"),
                    help="default tools/presets/xw.presets (committed); a copy always goes to "
                         "xbox360/out/xw.presets, next to the build, for eq.py install")
    ap.add_argument("--vectors", action="store_true", help="also write dist/eq_vectors.bin")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args()

    import custom  # here, not at the top: custom.py imports this module
    presets, flat_q15 = build(a.semantics, a.loudness)
    ctab = custom.build(flat_q15)
    data = pack(presets, flat_q15, ctab)
    back = roundtrip(data, presets, flat_q15, ctab)
    out_copy = os.path.join(ROOT, "xbox360", "out", "xw.presets")
    for path in (a.out, out_copy):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    if not a.quiet:
        flat_db = db(flat_q15 / 32768)
        print("%-3s %-20s %8s %8s %8s %6s %7s %9s  %s" % (
            "#", "name", "peak dB", "preamp", "level", "bands", "max L1", "quant dB", "file"))
        for i, p in enumerate(presets):
            x_max = (32768 * p["pre"] + 64) >> 7
            print("%-3d %-20s %+8.3f %+8.3f %+8.3f %6s %7.2f %9.5f  %s" % (
                i + 1, p["short"], p["peak"], db(p["pre"] / 32768), p["loud"] - flat_db,
                "".join("x" if p["active"] >> k & 1 else "." for k in range(5)),
                p["max_y"] / x_max, p["qerr"], p["rel"]))
            print("    %s%s" % (p["desc"], "   (own level)" if p["level"] == "own" else ""))
        print("semantics %s, loudness %s: FLAT %+.3f dB while the file is loaded (level is "
              "against it); every peak after preamp <= 0 dB (highest %+.3f); every band output "
              "< 2^31 (worst %.4f of it), every accumulator < 2^63 (worst 2^%.1f)" % (
                  a.semantics, a.loudness, flat_db, max(p["after"] for p in presets),
                  max(p["max_y"] for p in presets) / 2 ** 31,
                  math.log2(max(p["max_acc"] for p in presets))))
        print(custom.summary(ctab, flat_q15))
    print("wrote %s (and %s): %d presets + CUSTOM, %d bytes, crc %08x" % (
        shown(a.out), shown(out_copy), len(presets), len(data),
        struct.unpack(">I", data[24:28])[0]))
    if a.vectors:
        import vectors
        path = os.path.join(DIST, "eq_vectors.bin")
        os.makedirs(DIST, exist_ok=True)
        blob = vectors.build(presets, back, flat_q15, ctab)
        with open(path, "wb") as f:
            f.write(blob)
        print("wrote %s: %d bytes" % (shown(path), len(blob)))


if __name__ == "__main__":
    main()
