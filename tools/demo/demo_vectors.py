#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Vectors for the demo part of tests/test_game_surround.c: the game-audio tap with the demo
playing (surround_360.c, surround_demo.h, game_360.c), block by block, bit-exact.

    <venv with numpy>/bin/python -I tools/demo/demo_vectors.py   (writes tools/demo/demo_vectors.h)

The test clip (TEST_BLOCKS blocks of noise stems and random gains, made the same way here and
in the C test from xorshift streams; XDV_FILE_CRC proves both made the same bytes, and page
BAD_PAGE is damaged in the C test's file, so the console zeroes it) is played through a
schedule of actions taken before tap blocks: start, a surround switch, a stalled file worker
(the ring runs dry: underruns), its return, the clip's end, a second play stopped by a title
change, a third stopped by the player. The model: the driver's reads as surround_360.c's
dm_poll makes them (the C test runs every job a pass posts within that pass while the worker
is up), the tap's begin / fill / end, xdm_expand, the downmix of the virtualizer's input, and
the virtualizer and its switch from tools/hrtf/xsr_ref.py (as tools/hrtf/game_vectors.py).
"""
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "hrtf"))

import numpy as np  # noqa: E402

import xsr_ref  # noqa: E402

N = 256
SLOTS = 7          # surround_360.c DM_SLOTS
PAGE_BLOCKS = 8
READ_MIN = 2       # DM_READ_MIN
TEST_BLOCKS = 100  # the test clip: 13 pages, the last with 4 blocks of padding
BAD_PAGE = 5
SEED_STEM = 0x2545F491
SEED_GAIN = 0x9E3779B9
SEED_GAME = 0x510E527F
REC = 1048
PAGE = 8 + PAGE_BLOCKS * REC
HDR = 64
NONE, IN, PLAY, OUT = range(4)   # XSV_DM_*
A_START, A_STOP, A_TITLE, A_STALL, A_RESUME, A_SURR_ON, A_SURR_OFF = 1, 2, 4, 8, 16, 32, 64


def xorshift(state):
    s = state[0]
    s ^= (s << 13) & 0xFFFFFFFF
    s ^= s >> 17
    s ^= (s << 5) & 0xFFFFFFFF
    state[0] = s
    return s


def s16(v):
    return v - 65536 if v >= 32768 else v


def clip_records():
    """per block: (12 gains, stem A[256], stem B[256]), in the C test's draw order"""
    gs, ss = [SEED_GAIN], [SEED_STEM]
    out = []
    for _ in range(TEST_BLOCKS):
        g = []
        for k in range(12):
            x = xorshift(gs)
            v = x >> 17
            if (x >> 3) & 3 == 0:
                v = 0
            if k % 6 == 3:
                v >>= 3  # LFE: a little
            g.append(v)
        a = [s16(xorshift(ss) >> 16) >> 1 for _ in range(N)]
        b = [s16(xorshift(ss) >> 16) >> 1 for _ in range(N)]
        out.append((g, a, b))
    return out


def clip_bytes(recs):
    import struct
    blocks = len(recs)
    pages = (blocks + PAGE_BLOCKS - 1) // PAGE_BLOCKS
    hdr = struct.pack(">4sHHIHHHHIIIII20x", b"XWDM", 1, HDR, 48000, 6, 2, N, PAGE_BLOCKS, PAGE,
                      pages, blocks, blocks * N, 0)
    hdr += struct.pack(">I", zlib.crc32(hdr) & 0xFFFFFFFF)
    data = bytearray(hdr)
    for p in range(pages):
        body = bytearray(struct.pack(">I", p))
        for k in range(PAGE_BLOCKS):
            i = p * PAGE_BLOCKS + k
            if i < blocks:
                g, a, b = recs[i]
                body += struct.pack(">12h", *g) + struct.pack(f">{N}h", *a) + struct.pack(f">{N}h", *b)
            else:
                body += struct.pack(">12h", *recs[-1][0]) + bytes(4 * N)
        data += struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF) + body
    return bytes(data), pages


def expand(rec, prev, inp, mode):
    """surround_demo.h xdm_expand; rec None = silence. Returns (new input, new prev)"""
    n = np.arange(1, N + 1, dtype=np.int64)
    out = np.array(inp, dtype=np.int64)
    w = n if mode == IN else 256 - n
    for c in range(6):
        a0, b0 = prev[c], prev[6 + c]
        da = db = 0
        if rec is not None:
            da, db = rec[0][c] - a0, rec[0][6 + c] - b0
        if rec is None or not (a0 or b0 or da or db):
            out[c] = 0 if mode == PLAY else (out[c] * (256 - w)) >> 8
            continue
        x = np.array(rec[1], dtype=np.int64)
        y = np.array(rec[2], dtype=np.int64)
        v = (x * (a0 + ((da * n) >> 8)) + y * (b0 + ((db * n) >> 8))) >> 15
        v = np.clip(v, -32768, 32767)
        if mode != PLAY:
            v = (out[c] * (256 - w) + v * w) >> 8
        out[c] = v
    return out, (list(rec[0]) if rec is not None else prev)


def schedule():
    """actions before each tap block, as (block count, actions) runs"""
    runs = [(3, 0),                     # the game, gate on, surround off (the file is found)
            (1, A_START), (6, 0),       # IN, then the clip with surround off
            (1, A_SURR_ON), (9, 0),     # surround fades in on the clip
            (1, A_STALL), (59, 0),      # the worker stops: the ring runs dry, then silence
            (1, A_RESUME), (14, 0),     # back: the clip carries on where it stopped
            (1, A_SURR_OFF), (40, 0),   # surround fades out on the clip; it ends, the game back
            (1, A_SURR_ON), (3, 0),     # the game virtualized
            (1, A_START), (5, 0), (1, A_TITLE), (3, 0),   # a play stopped by a title change
            (1, A_START), (3, 0), (1, A_STOP), (3, 0)]    # and one stopped by the player
    acts = []
    for n, a in runs:
        acts += [a] + [0] * (n - 1) if a else [0] * n
    return acts


def model(tab, recs, pages):
    acts = schedule()
    nb = len(acts)
    zero_rec = ([0] * 12, [0] * N, [0] * N)
    rec_of = [zero_rec if i // PAGE_BLOCKS == BAD_PAGE else r for i, r in enumerate(recs)]
    game = np.zeros((6, nb * N), dtype=np.int64)
    for c in range(6):
        game[c] = xsr_ref.noise16((SEED_GAME + c * 0x9E3779B9) & 0xFFFFFFFF, nb * N)
    # the driver
    state, run, q, avail, pending, up = "idle", 0, 0, 0, None, True
    # the tap
    cur = faded = over = pos = 0
    prev = [0] * 12
    surround, s_cur, hist = 0, 0, None
    words, modes = [], []
    underruns = plays = 0

    def refill():
        nonlocal q, avail, pending
        while True:
            if pending is not None:
                if not up:
                    return
                q = avail = pending   # the job ran: the next poll publishes it
                pending = None
            done = pos // PAGE_BLOCKS
            free = SLOTS - (q - done)
            n = min(free, pages - q, SLOTS - q % SLOTS)
            if n <= 0 or not (state == "loading" or free >= READ_MIN or q + n == pages):
                return
            pending = q + n

    for b in range(nb):
        a = acts[b]
        if a & A_SURR_ON:
            surround = 1
        if a & A_SURR_OFF:
            surround = 0
        if a & A_STALL:
            up = False
        if a & A_RESUME:
            up = True
        if a & A_START:
            assert state == "idle"
            plays += 1
            state, q, avail, pending, pos, over = "loading", 0, 0, None, 0, 0
            refill()   # the C test passes until it plays (the worker is up)
            assert avail == min(SLOTS, pages)
            state, run = "playing", 1
        if a & (A_STOP | A_TITLE):
            run, state = 0, "stopping"
        # the pass before the block
        if state == "playing" and over:
            run, state = 0, "stopping"
        if state == "stopping" and not cur and pending is None:
            state = "idle"
        if state in ("playing", "loading"):
            refill()
        # the tap
        if not cur:
            mode = IN if run and not over else NONE
            if mode == IN:
                cur, faded = 1, 0
        else:
            mode = IN if not faded else PLAY if run and pos < TEST_BLOCKS else OUT
        q15 = xsr_ref.f2q15(np.asarray(_floats(game[:, b * N:(b + 1) * N])))[0]
        inp = xsr_ref.clamp16(q15)
        took = False
        if mode != NONE:
            if mode == IN:
                prev = [0] * 12
            rec = None
            if pos < TEST_BLOCKS and pos // PAGE_BLOCKS < avail:
                rec, took = rec_of[pos], True
            elif mode == PLAY:
                underruns += 1
            inp, prev = expand(rec, prev, inp, mode)
        plain = xsr_ref.downmix_q15(q15) if mode == NONE else xsr_ref.downmix(inp)
        # surround (the gate stays on)
        want = surround
        if not want and not s_cur:
            lr = plain
        else:
            s = np.asarray(inp, dtype=np.int64)
            if want and not s_cur:
                hist, how = s, "in"
            else:
                hist = np.concatenate([hist, s], axis=1)
                how = "out" if not want else "virtual"
            virt = xsr_ref.process(tab, hist)[0][:, -N:]
            if how == "in":
                lr, s_cur = xsr_ref.crossfade(plain, virt), 1
            elif how == "out":
                lr, s_cur = xsr_ref.crossfade(virt, plain), 0
            else:
                lr = virt
        words.append(xsr_ref.interleave(lr))
        modes.append(mode)
        if took:
            pos += 1
        if mode == IN:
            faded = 1
        elif mode == OUT:
            over, cur = 1, 0
    return acts, words, modes, underruns, plays, pos


def _floats(v):
    """s16 noise x 1.5 / 32768 as float32 bits (the C test's game frames)"""
    f = (np.asarray(v, dtype=np.float32) * np.float32(1.5) / np.float32(32768)).astype(np.float32)
    return f.view(np.uint32)


def main():
    recs = clip_records()
    data, pages = clip_bytes(recs)
    tabs = xsr_ref.parse_tab(os.path.join(ROOT, "xbox360", "surround_tab.h"))
    lines = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
             "/* GENERATED by tools/demo/demo_vectors.py: do not edit. tests/test_game_surround.c */",
             "#pragma once",
             f"#define XDV_CLIP_BLOCKS {TEST_BLOCKS}",
             f"#define XDV_BAD_PAGE {BAD_PAGE}",
             f"#define XDV_SEED_STEM 0x{SEED_STEM:08x}u",
             f"#define XDV_SEED_GAIN 0x{SEED_GAIN:08x}u",
             f"#define XDV_SEED_GAME 0x{SEED_GAME:08x}u",
             f"#define XDV_FILE_BYTES {len(data)}",
             f"#define XDV_FILE_CRC 0x{zlib.crc32(data) & 0xFFFFFFFF:08x}u"]
    first = True
    for taps, tab in tabs.items():
        acts, words, modes, under, plays, pos = model(tab, recs, pages)
        if first:
            lines += [f"#define XDV_BLOCKS {len(acts)}",
                      f"#define XDV_UNDERRUNS {under}",
                      f"#define XDV_PLAYS {plays}",
                      "enum { XDV_START = 1, XDV_STOP = 2, XDV_TITLE = 4, XDV_STALL = 8, "
                      "XDV_RESUME = 16, XDV_SURR_ON = 32, XDV_SURR_OFF = 64 };",
                      "static const u8 xdv_act[XDV_BLOCKS] = { " + ", ".join(map(str, acts)) + " };",
                      "/* XSV_DM_* the model's tap takes per block */",
                      "static const u8 xdv_mode[XDV_BLOCKS] = { " + ", ".join(map(str, modes)) + " };"]
        crcs = [zlib.crc32(w.astype(">i2").tobytes()) & 0xFFFFFFFF for w in words]
        lines += [f"#{'if' if first else 'elif'} XSR_TAPS == {taps}",
                  "static const u32 xdv_crc[XDV_BLOCKS] = { " + ", ".join(f"0x{c:08x}u" for c in crcs) + " };"]
        first = False
    lines += ["#endif", ""]
    out = os.path.join(HERE, "demo_vectors.h")
    with open(out, "w") as f:
        f.write("\n".join(lines))
    print("wrote", os.path.relpath(out, ROOT), f"({len(schedule())} blocks, {under} underruns)")


if __name__ == "__main__":
    main()
