#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screenshots of the console's front buffer over xbdm, de-tiled into a PNG, and a search for
the headset panel's 4 sentinel texels (hud_360.c). hud.py shot calls this; it also runs alone.

  shot.py FILE.png [--find] [--mem] [--swap-rb]
      FILE.png  the front buffer as a PNG (24-bit RGB, the buffer's own size)
      --find    say whether the panel's 4 sentinels are in the picture, and where; exit
                status 1 if they aren't
      --mem     read the frame with getmem at the HUD's last draw address (hud.py status
                d_base) instead of xbdm's screenshot command (a fallback: slower). Only from
                hud.py, which knows where that is.
      --swap-rb red and blue swapped, as hud.py set swap_rb 1
  shot.py --find          the same without writing a file

xbdm's screenshot (checked on this console's xbdm.xex): the frame the GPU scans out, at the
title's own size (1280x720 at Aurora; 864x480 was seen with the Guide open), tiled 8888.

A sentinel is the top-left pixel of one text unit of the panel whose colour differs from the
unit's other pixels by the low bit of blue, plus the low bit of red for sentinels 1 and 3 and
of green for 2 and 3 (hud_360.c make_ink). The search: a pixel whose right and lower
neighbours are equal, are one of the panel's text colours (PANEL_TEXT), and differ from it
by exactly that pattern; then the tightest set of all four inside a panel-sized box. It
needs the panel at scale 2 or more (720p and up): at scale 1 a unit is one pixel.

XBOX=host[:port] is the console's address (required). Shares xbdm code with xa.py.
"""
import array
import struct
import sys
import time
import zlib

from xa import Xbdm

# GPUTEXTUREFORMAT values the panel can be drawn into (hud_360.c decode_fetch)
FORMATS = {6: ("8_8_8_8", 8), 50: ("8_8_8_8_AS_16_16_16_16", 8), 7: ("2_10_10_10", 10),
           54: ("2_10_10_10_AS_16_16_16_16", 10)}
ENDIAN = {0: "none", 1: "8in16", 2: "8in32", 3: "16in32"}
SENTINELS = 4
# the colours a sentinel's unit can have (hud_360.c 4266a69: C_TEXT, C_WHITE, C_INK). If
# WP1.2's palette changes them, change this list too.
PANEL_TEXT = (0xc8c8c8, 0xf4f4f4, 0x101010)


class Frame:
    """One decoded frame: texel values (the GPU's view: endian swap applied) in row order,
    and how to take colour lanes out of them."""

    def __init__(self, width, height, texels, texfmt, swizzle, swap_rb=0):
        name, bits = FORMATS[texfmt]
        self.width, self.height, self.texels, self.bits = width, height, texels, bits
        self.format = name
        self.mask = (1 << bits) - 1
        src = [swizzle & 7, (swizzle >> 3) & 7, (swizzle >> 6) & 7]
        if any(c >= (4 if bits == 8 else 3) for c in src):
            raise SystemExit(f"swizzle {swizzle:03x}: a colour lane isn't a texel component")
        if swap_rb:
            src[0], src[2] = src[2], src[0]
        self.sr, self.sg, self.sb = (c * bits for c in src)

    def rgb(self, x, y):
        """native-depth (r, g, b) of pixel (x, y)"""
        t, m = self.texels[y * self.width + x], self.mask
        return t >> self.sr & m, t >> self.sg & m, t >> self.sb & m

    def png(self, path):
        sr, sg, sb, m, w = self.sr, self.sg, self.sb, self.mask, self.width
        down = 2 if self.bits == 10 else 0
        rows = []
        for y in range(self.height):
            row = self.texels[y * w:(y + 1) * w]
            out = bytearray(3 * w + 1)
            out[1::3] = bytes((t >> sr & m) >> down for t in row)
            out[2::3] = bytes((t >> sg & m) >> down for t in row)
            out[3::3] = bytes((t >> sb & m) >> down for t in row)
            rows.append(bytes(out))
        write_png(path, w, self.height, b"".join(rows))


def write_png(path, w, h, raw):
    """raw: h rows of (filter byte 0, w x RGB)"""
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def tiled_offset(x, y, pitch):
    """byte offset of texel (x, y) in a 2D tiled 32 bpp surface, pitch in texels (a multiple
    of 32): Xenia's texture_address::Tiled2D, the same function as hud_360.c's"""
    outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6
    inner = ((y >> 1) & 7) << 3 | (x & 7)
    b = (outer | inner) << 2
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return ((y & 1) << 4 | pipe << 6 | bank << 11 | (b & 15) | ((b >> 4) & 1) << 5 |
            ((b >> 5) & 7) << 8 | (b >> 8) << 12)


def decode(data, width, height, pitch, texfmt, endian, tiled, swizzle, swap_rb=0):
    """raw front-buffer bytes -> Frame. pitch in texels. The GPU reads memory little-endian,
    then applies the endian swap (hud_360.c to_texel)."""
    if texfmt not in FORMATS:
        raise SystemExit(f"texture format {texfmt}: not one the panel draws into "
                         f"({', '.join(f'{k} {v[0]}' for k, v in FORMATS.items())})")
    words = array.array("I")
    words.frombytes(data[:len(data) // 4 * 4])
    if sys.byteorder == "little":
        words.byteswap()  # now the CPU's (big-endian) view of each word
    if endian == 0:
        words.byteswap()
    elif endian == 1:
        words = array.array("I", ((w << 16 | w >> 16) & 0xffffffff for w in words))
    elif endian == 3:
        words = array.array("I", ((w & 0x00ff00ff) << 8 | (w >> 8 & 0x00ff00ff) for w in words))
    n = len(words)
    if tiled:
        if pitch % 32:
            raise SystemExit(f"tiled with a pitch of {pitch} texels (not a multiple of 32)")
        px = []
        for y in range(height):
            px.extend(words[i] if i < n else 0
                      for i in (tiled_offset(x, y, pitch) >> 2 for x in range(width)))
    else:
        px = []
        for y in range(height):
            row = words[y * pitch:y * pitch + width]
            px.extend(row)
            px.extend([0] * (width - len(row)))
    return Frame(width, height, px, texfmt, swizzle, swap_rb)


def d3dformat(f):
    """the D3DFORMAT word of xbdm's screenshot header -> (texfmt, endian, tiled, swizzle)"""
    return f & 0x3f, (f >> 6) & 3, (f >> 8) & 1, (f >> 18) & 0xfff


def grab(x=None, swap_rb=0):
    """the front buffer now, with xbdm's screenshot command: (Frame, header)"""
    hdr, data = (x or Xbdm(timeout=20)).screenshot()
    texfmt, endian, tiled, swizzle = d3dformat(hdr["format"])
    # xbdm sends each texel as a little-endian word, already in the GPU's view, whatever the
    # format's endian mode says (seen on the console: an 8in32 buffer came as R G B A bytes,
    # alpha 0xff last), so it is decoded with no endian swap
    return decode(data, hdr["width"], hdr["height"], hdr["pitch"] // 4, texfmt, 0, tiled,
                  swizzle, swap_rb), hdr


def fb_from_hud(g):
    """the last frame the HUD drew into, from hud.py's status block (d_* fields), or None"""
    if not g.get("draws") or not g.get("d_base"):
        return None
    return dict(base=g["d_base"], width=g["d_width"], height=g["d_height"], pitch=g["d_pitch"],
                texfmt=g["d_format"], endian=g["d_endian"], tiled=g["d_tiled"],
                swizzle=g["d_swizzle"])


def grab_mem(fb, x=None, swap_rb=0):
    """the frame at fb (fb_from_hud) read with getmem: the fallback if xbdm has no screenshot
    command. It is whatever that buffer holds now (the GPU may have resolved a newer frame)."""
    x = x or Xbdm()
    h = (fb["height"] + 31) & ~31 if fb["tiled"] else fb["height"]
    size, data = fb["pitch"] * h * 4, b""
    while len(data) < size:
        chunk = x.mem(fb["base"] + len(data), min(0x1000, size - len(data)))
        if chunk is None:
            raise SystemExit(f"getmem failed at {fb['base'] + len(data):08x}")
        data += chunk
    return decode(data, fb["width"], fb["height"], fb["pitch"], fb["texfmt"], fb["endian"],
                  fb["tiled"], fb["swizzle"], swap_rb)


def sentinel_pattern(fr, k):
    return 1 << fr.sb | (k & 1) << fr.sr | (k >> 1 & 1) << fr.sg


def native(fr, rgb):
    """an 8-bit-per-lane colour as the frame's lanes (hud_360.c texel_of)"""
    out = 0
    for v, sh in ((rgb >> 16 & 0xff, fr.sr), (rgb >> 8 & 0xff, fr.sg), (rgb & 0xff, fr.sb)):
        out |= (v << 2 | v >> 6 if fr.bits == 10 else v) << sh
    return out


def find_sentinels(fr, expect=None, bases=PANEL_TEXT):
    """{k: [(x, y, scale), ...]} candidates for each sentinel k: a pixel whose colour is one
    of the panel's text colours (bases) with sentinel k's bits flipped. scale is 2 or 3 if
    the pixel is the top-left of a unit of that size whose other pixels are the text colour
    itself, else 1. With expect = [(x, y)] (positions the HUD reports, if it does) only those
    pixels are checked."""
    w, h, px = fr.width, fr.height, fr.texels
    rgb = fr.mask << fr.sr | fr.mask << fr.sg | fr.mask << fr.sb
    sent = {native(fr, c) ^ sentinel_pattern(fr, k): (k, native(fr, c))
            for c in bases for k in range(SENTINELS)}
    found = {k: [] for k in range(SENTINELS)}
    want = set(expect or ())

    def unit(x, y, base):
        """the scale of the unit at (x, y): its pixels but the first are `base`"""
        f = 1
        for n in (2, 3):
            if x + n > w or y + n > h or any(px[(y + j) * w + x + i] & rgb != base
                                             for j in range(n) for i in range(n) if i or j):
                break
            f = n
        return f

    for y in range(h):
        row = px[y * w:(y + 1) * w]
        for x in range(w):
            hit = sent.get(row[x] & rgb)
            if hit and (not want or (x, y) in want):
                found[hit[0]].append((x, y, unit(x, y, hit[1])))
    return found


def best_set(found):
    """one candidate per sentinel: the tightest set of four at one scale and on one pixel grid
    (they are the panel's first text units in scan order): within 240 units across and 8
    down at scale 2 or 3; at scale 1, where a lone pixel's colour is the only evidence, on
    one row within 60 pixels. The largest scale wins. None if there is no such set."""
    for f in (3, 2, 1):
        cand = {k: [p for p in found[k] if p[2] == f] for k in range(SENTINELS)}
        if any(not cand[k] for k in range(SENTINELS)):
            continue
        dx, dy = (60, 0) if f == 1 else (240 * f, 8 * f)
        best = None
        for a in cand[0][:512]:
            pick = [a]
            for k in range(1, SENTINELS):
                near = [p for p in cand[k] if (p[0] - a[0]) % f == 0 and (p[1] - a[1]) % f == 0
                        and abs(p[0] - a[0]) < dx and abs(p[1] - a[1]) <= dy]
                if not near:
                    break
                pick.append(min(near, key=lambda p: abs(p[0] - a[0]) + abs(p[1] - a[1])))
            else:
                spread = (max(p[0] for p in pick) - min(p[0] for p in pick) +
                          max(p[1] for p in pick) - min(p[1] for p in pick))
                if best is None or spread < best[0]:
                    best = (spread, pick)
        if best:
            return best[1]
    return None


def report(fr, expect=None):
    """prints the sentinel search; the four (x, y, scale) found together, or None. Without
    expect, a set at scale 1 (lone pixels: the panel on a 480-line buffer, e.g. Aurora's
    864 x 480) is printed but not counted: ordinary pictures have such pixels too."""
    found = find_sentinels(fr, expect)
    pick = best_set(found)
    for k in range(SENTINELS):
        c = found[k]
        print(f"  sentinel {k}: {len(c)} candidate(s)" +
              (", first " + " ".join(f"({x},{y})" for x, y, _ in c[:4]) if c else ""))
    where = pick and (f"{' '.join(f'({x},{y})' for x, y, _ in pick)}; box "
                      f"({min(p[0] for p in pick)},{min(p[1] for p in pick)})-"
                      f"({max(p[0] for p in pick)},{max(p[1] for p in pick)})")
    if pick and (expect or pick[0][2] > 1):
        print(f"sentinels FOUND at scale {pick[0][2]}" +
              (" (at the HUD's positions)" if expect else "") + f": {where}")
        return pick
    if pick:
        print(f"sentinels NOT found at scale 2 or 3; a scale-1 match at {where} is not "
              f"conclusive (lone pixels): it needs the HUD's sentinel positions")
    else:
        print("sentinels NOT found")
    return None


def describe(fr, hdr=None):
    s = f"{fr.width}x{fr.height} {fr.format}"
    if hdr:
        texfmt, endian, tiled, swizzle = d3dformat(hdr["format"])
        s += (f", {ENDIAN[endian]}, {'tiled' if tiled else 'linear'}, swizzle {swizzle:03x} "
              f"(D3DFORMAT {hdr['format']:08x}), pitch {hdr['pitch']} bytes, display "
              f"{hdr.get('sw', 0)}x{hdr.get('sh', 0)}")
    return s


def cli(args, hud_status=None, sentinels=None):
    """hud.py shot / shot.py: args = [FILE.png] [--find] [--mem] [--swap-rb].
    hud_status() -> hud.py's status dict (for --mem); sentinels() -> the 4 sentinels' screen
    positions [(x, y)] * 4 if the HUD publishes them, else None (then the whole frame is
    searched). Exit status 1 if --find found nothing."""
    find, mem, swap_rb = "--find" in args, "--mem" in args, int("--swap-rb" in args)
    files = [a for a in args if not a.startswith("--")]
    bad = [a for a in args if a.startswith("--") and a not in ("--find", "--mem", "--swap-rb")]
    if bad or len(files) > 1 or not (files or find):
        raise SystemExit("usage: shot FILE.png [--find] [--mem] [--swap-rb] | shot --find")
    t = time.time()
    if mem:
        if not hud_status:
            raise SystemExit("--mem needs the HUD's last draw address: run it as hud.py shot --mem")
        fb = fb_from_hud(hud_status())
        if not fb:
            raise SystemExit("--mem: the HUD hasn't drawn yet, so the front buffer's address "
                             "isn't known")
        fr, hdr = grab_mem(fb, swap_rb=swap_rb), None
        print(f"getmem {fb['base']:08x}: " + describe(fr) +
              f", {ENDIAN.get(fb['endian'])}, {'tiled' if fb['tiled'] else 'linear'}")
    else:
        fr, hdr = grab(swap_rb=swap_rb)
        print("screenshot: " + describe(fr, hdr))
    if files:
        fr.png(files[0])
        print(f"{files[0]}: written ({time.time() - t:.1f} s)")
    if find and not report(fr, sentinels and sentinels()):
        sys.exit(1)


if __name__ == "__main__":
    cli(sys.argv[1:])
