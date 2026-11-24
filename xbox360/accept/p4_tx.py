#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for WP4.2, the team voice send path (docs/PLAN.md 4.3, docs/TEAM.md).

The Mac plays a teammate on channel 1 (UDP 3660 here) and records what the console sends,
while the console runs stage 4 on channel 1. Needs the pad on and its jack headset streaming
(xw.py audio 4, as for WP0.1): xt_mic is clocked by the headset's mic packets.

  XBOX=10.0.0.13 python3 xbox360/accept/p4_tx.py [--seconds 20] [--hands] [--wav FILE]
                                                 [--keep]

  --seconds N   how long the synthetic mic runs (default 20: 10 talk spurts)
  --hands       the owner is at the headset: the speech steps run (DO lines tell him when)
  --wav FILE    where the recording of his speech goes (default: a temp file, printed)
  --keep        leave stage 4 and channel 1 on at the end

Part 1, no hands (test_tone 2: a synthetic mic, 440 Hz at -20 dBFS, gated 1 s on / 1 s off
by the mic's own clock, through the whole send path: resampler, ramps, packets, the ring, the
net thread, the socket):
  - the Mac gets the console's AUDIO: 160-sample packets on channel 1, contiguous
    timestamps and sequence numbers inside every talk spurt (lost 0);
  - one spurt per second on, each 1 s long (+ at most 20 ms: the 10 ms fade and a packet),
    1 s apart; the tone at -20 dBFS (+/- 1 dB); every spurt starts and ends faded;
  - the console's counters agree: sent = what the Mac got, dropped 0, send failures 0, the
    gate's close to the last packet <= 30 ms;
  - MIC TO GAME sends nothing; BOTH sends; stage 3 sends nothing;
  - muted (a Share tap injected through XWPN) sends nothing and HELLO carries MUTED;
  - out_starved +0 (XWDG) and the mic still at >= 950 packets/s (XWAU).
Part 2, --hands (the real mic):
  - the owner talks 10 s: the recording has speech, and every talk spurt ends within 250 ms of
    the last 20 ms window over the gate's threshold (by the sender's own timestamps);
  - then he stays silent: tx sent flat (+0) over 4 s, from 1.5 s after the prompt;
  - muted while he talks: +0;
  - MIC TO TEAM: the 360's capture is silent (voice.py's capture peak stays 0) while the team
    gets it; BOTH: the 360's capture peak > 0 (needs a title that takes the headset: a Halo 3
    lobby or voice.py test; else FAIL with that reason).
  - OWNER CHECK: his recorded words are played back into his headset; they sound clean.
"""
import argparse
import math
import os
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import team  # noqa: E402
import voice  # noqa: E402
import xw  # noqa: E402
from accept import Checks, read_only  # noqa: E402
from xa import xw_block  # noqa: E402

PN_MAGIC = 0x5857504E  # "XWPN" (panel_360.c): inject is word 2, inject_pad word 3
PN_INJECT, PN_INJECT_PAD = 2, 3
SHARE = 1
TONE_AMP = 10 ** (-20 / 20)
WIN = 320  # 20 ms at 16 kHz: the gate's window


def info(text):
    print(f"INFO {text}", flush=True)


def do(text):
    print(f"DO   {text}", flush=True)


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


# ---- what the Mac recorded ------------------------------------------------------------------
def spurts_of(packets, sender):
    """the console's AUDIO packets split into talk spurts by timestamp. Each spurt: dict(start
    (timestamp, unwrapped from the first packet), pcm [samples], n packets, lost (timestamp
    holes inside it), seq_breaks, bad (not 160 samples / channel 1 / TALKING), arrivals)"""
    mine = [(h, p, t) for h, p, t in packets if h["sender"] == sender]
    out = []
    ts0 = mine[0][0]["timestamp"] if mine else 0
    prev = None
    for h, p, t in mine:
        rel = (h["timestamp"] - ts0) & 0xFFFFFFFF
        n = len(p) // 2
        pcm = list(struct.unpack(f">{n}h", p[:2 * n]))
        bad = h["samples"] != team.PACKET or n != team.PACKET or h["channel"] != 1 or \
            not h["flags"] & team.F_TALKING
        if prev is not None and 0 <= rel - prev["end"] < 5 * team.PACKET:
            sp = prev
            if rel != sp["end"]:
                sp["lost"] += (rel - sp["end"]) // team.PACKET
                sp["pcm"] += [0] * (rel - sp["end"])
            if (h["seq"] - sp["seq"]) & 0xFFFF != 1:
                sp["seq_breaks"] += 1
        else:
            sp = dict(start=rel, end=rel, pcm=[], n=0, lost=0, seq_breaks=0, bad=0, arrivals=[])
            out.append(sp)
            prev = sp
        sp["pcm"] += pcm
        sp["end"] = rel + n
        sp["seq"] = h["seq"]
        sp["n"] += 1
        sp["bad"] += 1 if bad else 0
        sp["arrivals"].append(t)
    return out


def max_gap_ms(arrivals):
    return max((b - a for a, b in zip(arrivals, arrivals[1:])), default=0) * 1000


def stop_after_ms(sp, threshold, gain_pct):
    """ms from the end of the spurt's last 20 ms window whose RMS (before the mic level) is
    at the gate's threshold or above, to the end of the spurt; None if no window was"""
    pcm = sp["pcm"]
    last = None
    for i in range(0, len(pcm) - WIN + 1, WIN // 4):
        w = pcm[i:i + WIN]
        rms = math.sqrt(sum(v * v for v in w) / WIN) * 100 / max(gain_pct, 1)
        if rms >= threshold:
            last = i + WIN
    return None if last is None else (len(pcm) - last) / 16


class Console:
    def __init__(self):
        self.x, self.addr = team.connect()
        self.vaddr = voice.block_addr()
        try:
            self.pn = xw_block(PN_MAGIC)
        except SystemExit:
            self.pn = None

    def team(self):
        return team.read_block(self.x, self.addr)

    def set(self, name, v):
        team.setctl(self.x, self.addr, name, v)

    def voice_pad(self, pad):
        g = voice.read_block(self.x, self.vaddr)
        return g["pads"][pad] if g else None

    def zero_mic_peak(self, pad):
        off = 4 * (len(voice.GLOBAL) + pad * len(voice.PAD) + voice.PAD.index("mic_peak"))
        self.x.setu32(self.vaddr + off, 0)

    def muted(self, pad):
        h = voice.read_hs(self.x)
        return h["pads"][pad]["muted"] if h else None

    def tap_share(self, pad):
        """a Share tap on pad, injected (panel_360.c): toggles its mic mute"""
        self.x.setu32(self.pn + 4 * PN_INJECT_PAD, pad)
        self.x.setu32(self.pn + 4 * PN_INJECT, SHARE)
        time.sleep(0.08)
        self.x.setu32(self.pn + 4 * PN_INJECT, 0)
        time.sleep(0.3)

    def sent(self):
        return self.team()["tx"]["sent"]


def sent_over(con, secs):
    a = con.sent()
    time.sleep(secs)
    return con.sent() - a


# ---- part 1: the synthetic mic --------------------------------------------------------------
def part_synthetic(c, con, bg, secs, sender):
    con.set("mic_route", 0)
    con.set("test_tone", 2)
    n0 = len(bg.packets)
    t0 = con.team()["tx"]
    time.sleep(secs)
    con.set("test_tone", 0)
    time.sleep(0.5)  # the last spurt's fade and the net thread
    t1 = con.team()["tx"]
    sps = spurts_of(bg.packets[n0:], sender)
    got = sum(sp["n"] for sp in sps)
    c.check("the Mac hears the console's AUDIO", got > 0,
            f"{got} packets in {len(sps)} spurts" if got else
            f"nothing (tx why: {team.TX_WHY.get(t1['why'], t1['why'])}, sent "
            f"{t1['sent'] - t0['sent']}; Mac firewall? the console's dest?)")
    if not got:
        return
    bad = sum(sp["bad"] for sp in sps)
    lost = sum(sp["lost"] for sp in sps)
    breaks = sum(sp["seq_breaks"] for sp in sps)
    c.check("packets: 160 samples, channel 1, TALKING", bad == 0, f"{bad} not")
    c.check("lost 0 inside the spurts", lost == 0 and breaks == 0,
            f"timestamp holes {lost} packets, sequence breaks {breaks}")
    inner = sps[1:-1]  # the first and last may be cut by the start and the end of the run
    want = secs / 2
    c.check("one spurt per 2 s", abs(len(sps) - want) <= 1.5, f"{len(sps)} (expected ~{want:.0f})")
    lens = [len(sp["pcm"]) for sp in inner]
    c.check("each spurt 1 s (+ <= 20 ms)", bool(lens) and all(16000 <= n <= 16320 for n in lens),
            f"{min(lens, default=0) / 16:.1f}..{max(lens, default=0) / 16:.1f} ms")
    gaps = [b["start"] - a["end"] for a, b in zip(sps, sps[1:])]
    c.check("1 s between spurts", bool(gaps) and all(15500 <= g <= 16500 for g in gaps),
            f"{min(gaps, default=0) / 16:.0f}..{max(gaps, default=0) / 16:.0f} ms")
    amps = [team.goertzel(sp["pcm"][320:-320], team.RATE, 440) for sp in inner]
    dbs = [20 * math.log10(a / TONE_AMP) if a > 0 else -99 for a in amps]
    c.check("440 Hz at -20 dBFS (+/- 1 dB)", bool(dbs) and all(abs(d) <= 1 for d in dbs),
            f"{min(dbs, default=-99):+.2f}..{max(dbs, default=-99):+.2f} dB")
    edges = [(abs(sp["pcm"][0]), abs(sp["pcm"][-1])) for sp in inner]
    c.check("spurts start and end faded (no click)", bool(edges) and
            all(a < 400 and b < 400 for a, b in edges), f"largest edge sample "
            f"{max((max(e) for e in edges), default=0)}")
    sent = t1["sent"] - t0["sent"]
    # (+/- 2: a packet or two in flight when the counters were read)
    c.check("console sent = Mac got", abs(sent - got) <= 2, f"sent {sent}, got {got}")
    c.check("ring: dropped 0", t1["dropped"] == t0["dropped"],
            f"dropped +{t1['dropped'] - t0['dropped']}, deepest {t1['depth_max']} of 32")
    g = con.team()
    c.check("send failures 0", g["send_fails"] == 0, f"{g['send_fails']} (last {g['err_send']})")
    c.check("gate close to the last packet <= 30 ms", t1["close_ms_max"] <= 30,
            f"{t1['close_ms_max']} ms")
    gm = [max_gap_ms(sp["arrivals"]) for sp in inner]
    info(f"arrival: largest gap inside a spurt {max(gm, default=0):.0f} ms (the net thread's "
         f"wake; wakes {t1['wakes'] - t0['wakes']}); tx sendto {g['tx_us_max']} us max")


def part_routes(c, con, bg, pad, sender):
    con.set("test_tone", 2)
    time.sleep(1)
    con.set("mic_route", 1)
    time.sleep(0.5)
    d = sent_over(con, 3)
    why = con.team()["tx"]["why"]
    c.check("MIC TO GAME: nothing sent", d == 0 and why == 3,
            f"+{d} packets, why {team.TX_WHY.get(why, why)}")
    # any 4 s holds 2 s of the synthetic mic's 1 s on / 1 s off: ~200 packets
    con.set("mic_route", 2)
    d = sent_over(con, 4)
    c.check("MIC TO BOTH: sent", d >= 150, f"+{d} packets in 4 s (expect ~200)")
    con.set("mic_route", 0)
    d = sent_over(con, 4)
    c.check("MIC TO TEAM: sent", d >= 150, f"+{d} packets in 4 s (expect ~200)")
    vp = con.voice_pad(pad)
    if vp and vp["serving"]:
        a = con.team()["tx"]["game_silenced"]
        time.sleep(1)
        b = con.team()["tx"]["game_silenced"]
        c.check("MIC TO TEAM: the 360 gets silence", b > a, f"xt_mic_to_game said silence "
                f"+{b - a} times in 1 s")
    else:
        info("no title is taking the headset (voice pad not serving): the 360-silence check "
             "is in --hands")
    con.set("stage", 3)
    time.sleep(0.5)
    d = sent_over(con, 3)
    c.check("stage 3: nothing sent", d == 0, f"+{d}")
    con.set("stage", 4)
    time.sleep(1)
    if con.pn is None:
        c.check("muted sends nothing", False, "no XWPN block to inject a Share tap")
        return
    m0 = con.muted(pad)
    if m0:
        con.tap_share(pad)  # start unmuted
    con.tap_share(pad)
    m = con.muted(pad)
    time.sleep(1.2)  # a HELLO with the flag
    d = sent_over(con, 3)
    fl = bg.hello_flags.get(sender, 0)
    con.tap_share(pad)
    c.check("muted: nothing sent, HELLO says MUTED", m == 1 and d == 0 and fl & team.F_MUTED,
            f"muted {m}, +{d} packets, HELLO flags {fl:x}")
    c.check("unmuted again", con.muted(pad) == 0, "")
    d = sent_over(con, 4)
    c.check("unmuted: sending again", d >= 150, f"+{d} packets in 4 s")
    con.set("test_tone", 0)
    if m0:
        con.tap_share(pad)


# ---- part 2: the owner ---------------------------------------------------------------------------
def countdown(text, lead=3):
    do(f"the owner: in {lead} s, {text}")
    time.sleep(lead)
    do("the owner: NOW")


def part_hands(c, con, bg, pad, sender, wav):
    d = xw.read_diag(con.x)
    thr, gain, gate = d["gate_threshold"], d["applied_mic_gain"], d["applied_gate"]
    info(f"mic level {gain} %, gate {xw.gate_name(gate)} (threshold RMS {thr})")
    con.set("mic_route", 0)
    con.set("test_tone", 0)
    if con.muted(pad):
        con.tap_share(pad)
    n0 = len(bg.packets)
    countdown("talk normally for 10 seconds (count slowly to ten)")
    time.sleep(10)
    do("the owner: now stay SILENT until told")
    time.sleep(1.5)
    flat = sent_over(con, 4)
    sps = spurts_of(bg.packets[n0:], sender)
    c.check("speech recorded", bool(sps) and sum(sp["n"] for sp in sps) >= 100,
            f"{len(sps)} spurts, {sum(sp['n'] for sp in sps)} packets")
    c.check("silence: sent flat", flat == 0, f"+{flat} packets in 4 s of silence")
    if gate == 0 or not thr:
        c.check("spurts end within 250 ms of silence", False, "the noise gate is off: the mic "
                "is sent all the time (xw.py diag gate 1)")
    else:
        ends = [stop_after_ms(sp, thr, gain) for sp in sps]
        ends = [e for e in ends if e is not None]
        c.check("spurts end within 250 ms of silence", bool(ends) and max(ends) <= 250,
                f"{', '.join(f'{e:.0f}' for e in ends)} ms after the last window over the "
                f"threshold")
    if sps:
        first = sps[0]["start"]
        mix = []
        for sp in sps:
            at = sp["start"] - first
            mix += [0] * (at - len(mix))
            mix += sp["pcm"]
        team.write_wav(wav, mix, team.RATE)
        info(f"recording: {len(mix) / team.RATE:.1f} s -> {wav}")
        do("the owner: listen: your own words come back in the headset now")
        replay(bg, sps)
        print("OWNER CHECK his words played back sound clean: no crackle, no gaps, no "
              "robot voice", flush=True)

    vp = con.voice_pad(pad)
    if not (vp and vp["serving"]):
        c.check("MIC TO TEAM / BOTH and the 360's capture", False, "no title takes the "
                "headset (voice pad not serving): start a Halo 3 lobby or voice.py test, then "
                "run --hands again")
    else:
        for route, name in ((0, "TEAM"), (2, "BOTH")):
            con.set("mic_route", route)
            time.sleep(0.3)
            con.zero_mic_peak(pad)
            a = con.sent()
            countdown(f"talk for 5 seconds (MIC TO {name})", 2)
            time.sleep(5)
            peak = con.voice_pad(pad)["mic_peak"]
            dsent = con.sent() - a
            if route == 0:
                c.check("MIC TO TEAM: the 360's capture silent, the team gets it",
                        peak == 0 and dsent > 0, f"capture peak {peak}, +{dsent} packets")
            else:
                c.check("MIC TO BOTH: the 360's capture has it too", peak > 0 and dsent > 0,
                        f"capture peak {peak}, +{dsent} packets")
        con.set("mic_route", 0)
    if con.pn is not None:
        con.tap_share(pad)
        m = con.muted(pad)
        a = con.sent()
        countdown("talk for 5 seconds (you are MUTED)", 2)
        time.sleep(5)
        dsent = con.sent() - a
        con.tap_share(pad)
        c.check("muted while talking: nothing sent", m == 1 and dsent == 0,
                f"muted {m}, +{dsent} packets")
    do("the owner: done, thank you")


def replay(bg, sps):
    """the recorded spurts back to the console, as another peer on channel 1, at their pace"""
    rid = bg.peer.sender ^ 0x2000
    start = time.monotonic()
    first = sps[0]["start"]
    for sp in sps:
        pcm = sp["pcm"]
        for i in range(0, len(pcm) - team.PACKET + 1, team.PACKET):
            due = start + (sp["start"] - first + i) / team.RATE
            while time.monotonic() < due:
                time.sleep(0.001)
            chunk = pcm[i:i + team.PACKET]
            bg.peer.send(team.AUDIO, 1, len(chunk), sp["start"] + i,
                         struct.pack(f">{len(chunk)}h", *chunk), team.F_TALKING, sender=rid)
    time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--wav", default=None)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    c = Checks()
    try:
        con = Console()
        g = con.team()
    except SystemExit as e:
        c.check("XWTV block", False, str(e))
        c.done()
    c.check("XWTV block version 2 (the send path)", g["tx"] is not None,
            f"version {g['version']} at {con.addr:08x}")
    if g["tx"] is None:
        c.done()
    au = xw.read_status(con.x, xw.status_addr()).get("audio")
    streaming = bool(au) and au["mic_pps"] > 0
    c.check("headset streaming", streaming, f"mic_pps {au['mic_pps'] if au else '-'}" +
            ("" if streaming else " (pad on? headset in? xw.py audio 4?)"))
    if read_only() or not streaming:
        team.print_status(g)
        c.done()
    saved = {k: g[k] for k in ("stage", "channel", "mic_route", "test_tone")}
    d0 = xw.read_diag(con.x)
    bg = team.Background(channel=1, name="P4TX", keep_audio=True).start()
    try:
        con.set("channel", 1)
        con.set("stage", 4)
        for name in team.COUNTERS + team.TX_COUNTERS:
            con.set(name, 0)
        end = time.time() + 10
        g = con.team()
        while time.time() < end and g["net_state"] != 1:
            time.sleep(0.25)
            g = con.team()
        c.check("socket open at stage 4", g["net_state"] == 1,
                team.NET_STATES.get(g["net_state"], g["net_state"]))
        sender = g["sender"]
        time.sleep(1)
        pad = s32(con.team()["tx"]["mic_pad"])
        if pad < 0:
            c.check("xt_mic called (the hook in voice_360.c)", False, "mic_pad -1: no calls")
            c.done()
        info(f"console {team.ip_str(g['ip'])} id {sender:08x}; mic on pad {pad}")
        part_synthetic(c, con, bg, a.seconds, sender)
        part_routes(c, con, bg, pad, sender)
        if a.hands:
            wav = a.wav or os.path.join(tempfile.mkdtemp(prefix="p4tx"), "speech.wav")
            part_hands(c, con, bg, pad, sender, wav)
        d1 = xw.read_diag(con.x)
        c.check("out_starved +0", d0 is not None and d1 is not None and
                d1["out_starved"] == d0["out_starved"],
                f"{d0['out_starved'] if d0 else '-'} -> {d1['out_starved'] if d1 else '-'}")
        au = xw.read_status(con.x, xw.status_addr()).get("audio")
        c.check("mic still at 1000 packets/s", bool(au) and au["mic_pps"] >= 950,
                f"mic_pps {au['mic_pps'] if au else '-'}")
        tx = con.team()["tx"]
        info(f"tx: {tx['spurts']} spurts, {tx['queued']} queued, {tx['sent']} sent, "
             f"{tx['discarded']} discarded, {tx['dropped']} dropped, deepest {tx['depth_max']}, "
             f"peak {tx['peak']}, net wakes {tx['wakes']}")
        if not a.hands:
            print("OWNER CHECK (run again with --hands, the owner at the headset, a Halo 3 lobby "
                  "open): his speech reaches the Mac and plays back clean; silence, mute and "
                  "MIC TO TEAM send / give the 360 nothing", flush=True)
    finally:
        try:
            con.set("test_tone", 0)
            if con.pn is not None:
                con.x.setu32(con.pn + 4 * PN_INJECT, 0)
            if not a.keep:
                for k, v in saved.items():
                    con.set(k, v)
                info(f"restored {saved}")
        finally:
            bg.stop()
    c.done()


if __name__ == "__main__":
    main()
