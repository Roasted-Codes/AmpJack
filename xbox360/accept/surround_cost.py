#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console measurement for headphone surround (docs/ROADMAP.md 6.1, M1-M4): does the 360 send
real 5.1 into the tap, and what does the virtualizer cost where it would run.

Needs an XW_DEV build (the dummy load is the probe; a release build is refused with a clear
message), the pad on with the jack headset streaming, and a game playing (Halo 3 in a match
for the timing rows). Run:

  XBOX=10.0.0.13 python3 xbox360/accept/surround_cost.py [--seconds 60] [--only M1,M3]

Rows, each SECONDS long (the roadmap's 60 s by default):
  M1  gate on, surround off: C, SL and SR peaks during gameplay. PASS: any of them above
      -50 dBFS (else: set the console to Dolby Digital 5.1 and repeat; a per-console setup
      step at the LAN if that is what it takes).
  M2  gate on, no load: the baseline tap time and period, and every counter M3 / M4 compare.
  M3  dummy load in the tap, 5x32, 5x64, 10x64, 10x128 (and the real engine, surround on):
      PASS when tap p99 <= 300 us and max <= 500 us, tap period max within +1 ms of M2,
      capture errors +0, overflow frames +0, tap rate 187-188 a second, frames shown a
      second not more than 1 below M2.
  M4  the same sizes on the driver thread (per earpiece block): PASS when XWDG work max
      rises <= 1500 us, passes >= 25 ms +0, earpiece starved +0, frames missed +0, game
      underruns +0, mic packets ~1000 a second.
Then the decision of 6.1: 5x64 passes M3 -> design A (built); only 5x32 -> A with 32 taps;
M3 fails but M4 passes -> design B; neither -> 6.4.

ACCEPT_READ_ONLY=1: only M1's reads. Everything written is put back at the end.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
sys.path.insert(0, XBOX360)

from accept import Checks, read_only  # noqa: E402

SIZES = [(5, 32), (5, 64), (10, 64), (10, 128)]
SAMPLE_S = 0.1
LIVE_PEAK = 104  # -50 dBFS


def tool():
    import importlib.util
    spec = importlib.util.spec_from_file_location("surround_tool", os.path.join(XBOX360, "surround.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def counters(x):
    """XVGA / XWDG / XWAU / XH36 counters by name (accept/overhead.py's reading)"""
    import overhead
    blocks, _ = overhead.read_blocks(x)
    out = overhead.flat(blocks)
    g = blocks.get("XVGA") or {}
    for k in ("capture_errors", "overflow_frames", "tap_rate", "tap_on"):
        if k in g:
            out[k] = g[k]
    return out


def run(x, addr, st, seconds):
    """one row: tap_us sampled every SAMPLE_S, and the counters before / after"""
    st.write(x, addr, "zero", 1)
    time.sleep(0.2)
    c0 = counters(x)
    samples, rates, swaps, pps = [], [], [], []
    end = time.time() + seconds
    while time.time() < end:
        d = st.read(x, addr)
        samples.append(d["tap_us"])
        c = counters(x)
        rates.append(c.get("tap_rate", 0))
        if "swaps_per_s" in c:
            swaps.append(c["swaps_per_s"])
        if "mic_pps" in c:
            pps.append(c["mic_pps"])
        time.sleep(SAMPLE_S)
    d = st.read(x, addr)
    c1 = counters(x)
    s = sorted(samples) or [0]
    r = dict(p99=s[min(len(s) - 1, int(len(s) * 0.99))], max=d["tap_max_us"],
             period=d["tap_period_max_us"], hist=d["tap_hist"], n=len(samples),
             rate_min=min(rates or [0]), rate_max=max(rates or [0]),
             swaps_min=min(swaps) if swaps else None, pps_min=min(pps) if pps else None)
    for k in ("capture_errors", "overflow_frames", "out_starved", "out_frames_missed",
              "underruns"):
        r[k] = c1.get(k, 0) - c0.get(k, 0)
    r["work_max_us"] = c1.get("work_max_us", 0)
    h0, h1 = c0.get("pass_hist", [0] * 6), c1.get("pass_hist", [0] * 6)
    r["passes_ge25"] = h1[5] - h0[5]
    return r


def line(name, r):
    return (f"{name}: tap p99 {r['p99']} us (of {r['n']} samples), max {r['max']} us, period max "
            f"{r['period']} us, hist {r['hist']}; capture errors +{r['capture_errors']}, overflow "
            f"+{r['overflow_frames']}, tap rate {r['rate_min']}-{r['rate_max']}/s, frames shown "
            f"min {r['swaps_min']}/s; driver work max {r['work_max_us']} us, passes >=25 ms "
            f"+{r['passes_ge25']}, starved +{r['out_starved']}, missed +{r['out_frames_missed']}, "
            f"underruns +{r['underruns']}, mic min {r['pps_min']}/s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--only", default="M1,M2,M3,M4")
    args = ap.parse_args()
    only = set(args.only.upper().split(","))
    st = tool()
    c = Checks()
    x, addr, d = st.connect()
    st.need_dev(d)
    saved = {k: d[k] for k in ("enable", "surround", "load_convs", "load_taps", "load_site", "test_ch")}
    base = None
    m3 = {}
    try:
        if "M1" in only:
            if not read_only():
                st.write(x, addr, "enable", 1)
                st.write(x, addr, "surround", 0)
                st.write(x, addr, "zero", 1)
            time.sleep(args.seconds)
            d = st.read(x, addr)
            pk = d["ch_peak_max"]
            c.check("M1 the tap carries 5.1 (C, SL or SR above -50 dBFS)",
                    max(pk[2], pk[4], pk[5]) > LIVE_PEAK,
                    "peak maxima " + ", ".join(f"{n} {st.dbfs(p)}" for n, p in zip(st.CHANNELS, pk)))
        if read_only():
            c.done()  # exits; the finally below writes nothing back in a dry run
        st.write(x, addr, "enable", 1)
        st.write(x, addr, "surround", 0)
        st.write(x, addr, "load_convs", 0)
        if only & {"M2", "M3", "M4"}:
            base = run(x, addr, st, args.seconds)
            c.check("M2 baseline recorded (game audio streaming)", base["rate_min"] >= 187,
                    line("M2", base))
        for site, row in ((0, "M3"), (1, "M4")):
            if row not in only:
                continue
            for convs, taps in SIZES:
                for k, v in (("load_site", site), ("load_taps", taps), ("load_convs", convs)):
                    st.write(x, addr, k, v)
                r = run(x, addr, st, args.seconds)
                st.write(x, addr, "load_convs", 0)
                name = f"{row} {convs}x{taps}"
                if site == 0:
                    ok = (r["p99"] <= 300 and r["max"] <= 500 and r["period"] <= base["period"] + 1000
                          and r["capture_errors"] == 0 and r["overflow_frames"] == 0
                          and r["rate_min"] >= 187 and r["rate_max"] <= 188
                          and (r["swaps_min"] is None or base["swaps_min"] is None
                               or r["swaps_min"] >= base["swaps_min"] - 1))
                    m3[(convs, taps)] = ok
                else:
                    ok = (r["work_max_us"] - base["work_max_us"] <= 1500 and r["passes_ge25"] == 0
                          and r["out_starved"] == 0 and r["out_frames_missed"] == 0
                          and r["underruns"] == 0 and (r["pps_min"] is None or r["pps_min"] >= 990))
                c.check(name, ok, line(name, r))
            if site == 0:
                st.write(x, addr, "surround", 1)  # the real engine, for the record
                time.sleep(0.5)
                r = run(x, addr, st, args.seconds)
                st.write(x, addr, "surround", 0)
                c.check(f"M3 real engine ({d['taps']} taps, surround on)",
                        r["p99"] <= 300 and r["max"] <= 500 and r["overflow_frames"] == 0,
                        line("engine", r))
        if m3:
            if m3.get((5, 64)):
                verdict = "design A at 64 taps (as built)"
            elif m3.get((5, 32)):
                verdict = "design A with 32 taps (rebuild with XSR_TAPS=32)"
            else:
                verdict = "M3 fails: design B if M4 passed, else ROADMAP 6.4"
            print(f"decision (ROADMAP 6.1): {verdict}")
    finally:
        if not read_only():
            for k, v in saved.items():
                st.write(x, addr, k, v)
    c.done()


if __name__ == "__main__":
    main()
