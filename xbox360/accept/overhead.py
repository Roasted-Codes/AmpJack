#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The "low overhead" scorecard (docs/ROADMAP.md Part 2 C.4, docs/QUALITY.md): the plugin's
cost read into one table, and compared with a stored baseline.

  XBOX=10.0.0.13 python3 xbox360/accept/overhead.py [--seconds N] [--save FILE]
                                                    [--compare FILE] [--dry-run]

  --seconds N     how long the console is watched (default 60; C.4 asks for a 15-minute
                  match: 900). Counters are read every 5 s; the table shows what rose over
                  the window, averages of the per-second figures, and the maxima at the end
  --save FILE     store the table's numbers as a baseline (JSON)
  --compare FILE  one PASS / FAIL line per measure against that baseline (C.4's targets);
                  the exit status is 1 if any failed
  --dry-run       no console: the table from a canned reading (for checking this script)

Rows, and where they come from (each block is read by the tool that owns its layout):
  image, code, constants, data   xbox360/out/xw.exe, the local build (tools/size_report.py)
  driver work per pass           XWDG work_us (average over the samples), work_max_us
  passes by length               XWDG pass_hist, the rise over the window; >= 25 ms is C.4's
                                 "no new long passes"
  dropouts                       XWDG out_starved, out_frames_missed; XVGA game underruns
                                 (rise over the window)
  mic packets per second         XWAU mic_pps (min / average / max; 1000 with a headset)
  picture                        XH36 draw time (d_ticks, d_ticks_max: timebase ticks, 50 a
                                 microsecond), swaps_per_s, pres_pct
A block the running build does not have (an older build, or a module not loaded) leaves its
rows empty with the reason; it is never read with a guessed layout.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
sys.path.insert(0, XBOX360)
sys.path.insert(0, os.path.join(os.path.dirname(XBOX360), "tools"))

from accept import Checks  # noqa: E402

TB_PER_US = 50  # timebase ticks per microsecond (hud.py prints d_ticks / 50)
SAMPLE_S = 5
LONG_PASS = 5  # xw.PASS_HIST index of ">=25" ms


def image_numbers(exe=None):
    """the local build's size: {image, code, const, data}, or None without a build"""
    import size_report
    exe = exe or os.path.join(XBOX360, "out", "xw.exe")
    if not os.path.exists(exe):
        return None
    pe = size_report.pe_read(exe)
    return dict(image=pe["size"], **size_report.pe_split(pe))


def read_blocks(x):
    """one reading of every block the scorecard needs: {name: dict or None}, and why a block
    is missing. Uses xw.py / voice.py / hud.py, which own the block layouts."""
    import hud
    import voice
    import xw
    blocks, why = {}, {}

    def get(name, fn):
        try:
            blocks[name] = fn()
        except SystemExit as e:  # the tools exit with a message when a block is not there
            blocks[name], why[name] = None, str(e)
        except OSError as e:
            blocks[name], why[name] = None, f"console: {e}"
        if blocks[name] is None and name not in why:
            why[name] = "not in the running build (older build?)"

    get("XWDG", lambda: xw.read_diag(x, quiet=True))
    get("XWAU", lambda: (xw.read_status(x, xw.status_addr()) or {}).get("audio"))
    get("XVGA", lambda: voice.read_ga(x, voice.ga_addr()))
    get("XH36", lambda: hud.read_block(x, hud.block_addr()))
    return blocks, why


def flat(blocks):
    """the counters of one reading, by name (absent when their block is)"""
    d, a, g, h = (blocks.get(k) for k in ("XWDG", "XWAU", "XVGA", "XH36"))
    out = {}
    if d:
        out.update(work_us=d["work_us"], work_max_us=d["work_max_us"],
                   pass_hist=list(d["pass_hist"]), out_starved=d["out_starved"],
                   out_frames_missed=d["out_frames_missed"])
    if a:
        out["mic_pps"] = a["mic_pps"]
    if g:
        out["underruns"] = g["underruns"]
    if h:
        out.update(hud_draw_us=h["d_ticks"] // TB_PER_US,
                   hud_draw_max_us=h["d_ticks_max"] // TB_PER_US)
        for k in ("swaps_per_s", "pres_pct"):  # XH36 version 2 / version 9 and later
            if k in h:
                out[k] = h[k]
    return out


# the rows: (key, label, unit, block it needs)
ROWS = [
    ("image", "image size", "bytes", "build"),
    ("code", "  code", "bytes", "build"),
    ("const", "  constants", "bytes", "build"),
    ("data", "  data", "bytes", "build"),
    ("work_us", "driver work per pass, average", "us", "XWDG"),
    ("work_max_us", "driver work per pass, max", "us", "XWDG"),
    ("passes", "driver passes in the window", "", "XWDG"),
    ("passes_ge25", "  of them >= 25 ms", "", "XWDG"),
    ("out_starved", "earpiece starved (rise)", "", "XWDG"),
    ("out_frames_missed", "earpiece frames missed (rise)", "", "XWDG"),
    ("underruns", "game audio underruns (rise)", "", "XVGA"),
    ("mic_pps_min", "mic packets/s, min", "/s", "XWAU"),
    ("mic_pps", "mic packets/s, average", "/s", "XWAU"),
    ("hud_draw_us", "panel draw, last", "us", "XH36"),
    ("hud_draw_max_us", "panel draw, max", "us", "XH36"),
    ("swaps_per_s", "frames shown/s, min", "/s", "XH36"),
    ("pres_pct", "panel present in scan-out", "%", "XH36"),
]


def summarise(image, samples):
    """the table's numbers from the image size and a list of flat() readings: {key: value};
    a key is absent when its counter is"""
    r = dict(image or {})
    if not samples:
        return r
    first, last = samples[0], samples[-1]
    col = lambda k: [s[k] for s in samples if k in s]
    if col("work_us"):
        r["work_us"] = sum(col("work_us")) // len(col("work_us"))
    for k in ("work_max_us", "hud_draw_us", "hud_draw_max_us", "pres_pct"):
        if k in last:
            r[k] = last[k]
    if "pass_hist" in first and "pass_hist" in last:
        rise = [b - a for a, b in zip(first["pass_hist"], last["pass_hist"])]
        r["passes"], r["passes_ge25"] = sum(rise), rise[LONG_PASS]
    for k in ("out_starved", "out_frames_missed", "underruns"):
        if k in first and k in last:
            r[k] = last[k] - first[k]
    if col("mic_pps"):
        r["mic_pps_min"] = min(col("mic_pps"))
        r["mic_pps"] = sum(col("mic_pps")) // len(col("mic_pps"))
    if col("swaps_per_s"):
        r["swaps_per_s"] = min(col("swaps_per_s"))
    return r


def print_table(r, why, seconds):
    print(f"overhead over {seconds} s" if seconds else "overhead (canned reading)")
    for key, label, unit, block in ROWS:
        v = r.get(key)
        if v is None:
            reason = "no local build" if block == "build" else why.get(block, "not read")
            print(f"  {label:<34} {'-':>9}     {block}: {reason}")
        else:
            hexed = f" ({v:#x})" if key == "image" else ""
            print(f"  {label:<34} {v:>9} {unit:<3} {block}{hexed}")


# C.4's targets: key -> (test(new, base), what it means). base is the stored baseline's value.
TARGETS = {
    "image": (lambda n, b: n <= b and n < 0x100000, "no bigger than the baseline, under 0x100000"),
    "work_us": (lambda n, b: n - b < 500, "average driver work rises less than 0.5 ms"),
    "passes_ge25": (lambda n, b: n == 0, "no new driver passes of 25 ms or more"),
    "out_starved": (lambda n, b: n <= b, "no more earpiece starvation than the baseline"),
    "out_frames_missed": (lambda n, b: n <= b, "no more missed earpiece frames"),
    "underruns": (lambda n, b: n <= b, "no more game audio underruns"),
    "mic_pps_min": (lambda n, b: abs(n - b) <= 2, "mic packets/s within 2 of the baseline"),
    "swaps_per_s": (lambda n, b: n >= b - 1, "frame rate unchanged"),
    "hud_draw_max_us": (lambda n, b: n < 1000, "panel draw under 1 ms"),
}


def compare(r, base, why):
    c = Checks()
    for key, label, unit, block in ROWS:
        if key not in TARGETS:
            if key in r and key in base:
                print(f"INFO {label.strip()}: {base[key]} -> {r[key]} {unit}".rstrip(), flush=True)
            continue
        test, meaning = TARGETS[key]
        if key not in r or key not in base:
            side = "now" if key not in r else "in the baseline"
            reason = why.get(block, "") if key not in r else ""
            c.check(meaning, False, f"{label.strip()}: missing {side}" + (f" ({reason})" if reason else ""))
            continue
        c.check(meaning, test(r[key], base[key]), f"{label.strip()} {base[key]} -> {r[key]} {unit}".rstrip())
    c.done()


# a plausible reading for --dry-run: two samples a window apart (not measured values)
CANNED_IMAGE = dict(image=0xde000, code=245840, const=23276, data=627740)
CANNED = [
    dict(XWDG=dict(work_us=410, work_max_us=2900, pass_hist=[52000, 900, 40, 3, 0, 0],
                   out_starved=0, out_frames_missed=0), XWAU=dict(mic_pps=1000),
         XVGA=dict(underruns=0), XH36=dict(d_ticks=21000, d_ticks_max=38000, swaps_per_s=30,
                                           pres_pct=100)),
    dict(XWDG=dict(work_us=430, work_max_us=2900, pass_hist=[64100, 1100, 48, 3, 0, 0],
                   out_starved=0, out_frames_missed=0), XWAU=dict(mic_pps=999),
         XVGA=dict(underruns=0), XH36=dict(d_ticks=20500, d_ticks_max=38000, swaps_per_s=30,
                                           pres_pct=100)),
]


def watch(seconds):
    """reads the console for `seconds`: (flat readings, why blocks are missing)"""
    from xa import Xbdm
    try:
        x = Xbdm()
    except OSError as e:
        raise SystemExit(f"console {os.environ.get('XBOX')}: {e}")
    samples, why = [], {}
    end = time.time() + seconds
    while True:
        blocks, w = read_blocks(x)
        why.update(w)
        samples.append(flat(blocks))
        if time.time() >= end:
            return samples, why
        time.sleep(min(SAMPLE_S, max(0.0, end - time.time())))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--save", metavar="FILE")
    ap.add_argument("--compare", metavar="FILE")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.dry_run:
        image, samples, why, secs = CANNED_IMAGE, [flat(s) for s in CANNED], {}, 0
    else:
        image = image_numbers()
        samples, why = watch(a.seconds)
        secs = a.seconds
    r = summarise(image, samples)
    print_table(r, why, secs)
    if a.save:
        with open(a.save, "w") as f:
            json.dump(r, f, indent=1, sort_keys=True)
        print(f"saved {a.save}")
    if a.compare:
        with open(a.compare) as f:
            compare(r, json.load(f), why)
    # a plain reading fails only when the console gave none of the blocks
    sys.exit(0 if any(k in r for k in TARGETS if k != "image") else 1)


if __name__ == "__main__":
    main()
