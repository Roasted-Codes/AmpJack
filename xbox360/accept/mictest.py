#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Console acceptance for the MIC TEST row, version 2 (record / play / pause): record the mic
as teammates would hear it (after MIC VOLUME, the noise gate and the limiter) for as long as
you like (auto-stop 30 s), hear it once, pause, resume, play it again; the clip is deleted,
and its memory freed, when the panel closes with nothing recording or playing.

Run it with an xw.xex that has XWMT version 2, the pad on and the jack headset plugged in and
streaming (any title; Aurora is fine). Run it once more with Halo 3 in a match: that is the
probe of the 1.44 MB allocation under a game.

  XBOX=10.0.0.13 python3 xbox360/accept/mictest.py [--pad N] [--hands] [--wav FILE]

  --pad N     the pad slot of the headset (default 0)
  --hands     also run the the owner check (y / n prompt; without it, a TODO line)
  --wav FILE  where to save a short clip (default out/mictest.wav)

ACCEPT_READ_ONLY=1 (accept/run.py --read-only): only the reads (step 1), nothing written.

Everything is driven over xbdm: the panel through XWPN inject (Share, Down, Left, Right), the
rest through XWMT cmd. Clips are short (XWMT rec_ms 1000..3000, put back to 30000 at the end),
so the run takes about two minutes. The mic is muted and unmuted once (the bar shows it).

Checks (one PASS or FAIL line each; exit status 1 if any failed):
  1. XWMT version 2, XWDG version 3, XWPN version 5; defaults: auto-stop 30000 ms, duck 25 %,
     on, idle with nothing allocated;
  2. enable 0: a start is refused (switched off) and the MIC TEST row is not reachable;
  3. through the panel: Right on MIC TEST records (1.44 MB allocated, or the 10 s fallback
     with the auto-stop at 10 s), the seconds count up; Share let go: still recording, the
     toast shows; Share again: the cursor is on MIC TEST; Right: it plays, the game ducked;
     Right: PAUSED, the position holds, the game back; Right: PLAYING on from there; Share
     let go: it plays to the end -> READY, all of it played, never past full scale, clip held;
     the panel again, Right: it plays again with no new allocation; READY; Share let go with
     the clip held: IDLE, the memory freed;
  4. Left while recording / playing / paused (xbdm cancel): IDLE, freed, the game back;
  5. the auto-stop (1 s): it plays by itself; a 0.2 s recording is discarded, not played;
  6. a mute while recording ends it and plays it; a start while muted is refused;
  7. a clip that finishes while the panel is hidden is kept until the panel has been opened
     and closed; the panel closing while PAUSED deletes the clip;
  8. a clip saved as a WAV (24 kHz mono), its length as recorded; XWMT version 3: voice.py
     mictest record 2 s saves a clip that was never played, start-hold ends READY unplayed,
     and keep makes a panel close leave the clip (kept counts it); while a clip plays, Up from
     MIC TEST is NOISE GATE and a step there keeps it playing, through the new step;
  9. XW_DEV builds: the 10 s fallback and the out-of-memory refusal (forced), then 1.44 MB
     again; release builds: skipped (INFO);
 10. settings put back, nothing allocated.
the owner check (--hands):
  J1 Hold Share, go to MIC TEST, press Right and talk for as long as you like, press Right to
     stop and hear it, Right to pause, Right to carry on, then let it finish and press Right
     to hear it again; let go of Share and open the panel again: the clip is gone.
"""
import argparse
import os
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

import hud  # noqa: E402
import voice  # noqa: E402
import wp1_3 as wp  # noqa: E402  (Console: XWPN inject, the panel's reads)
import xw  # noqa: E402
from xa import xw_block  # noqa: E402

failed, passed = [], []
SHARE, UP, DOWN, LEFT, RIGHT = wp.SHARE, wp.UP, wp.DOWN, wp.LEFT, wp.RIGHT
IDLE, RECORDING, PLAYING, PAUSED, READY = (voice.IDLE, voice.RECORDING, voice.PLAYING,
                                           voice.PAUSED, voice.READY)
CMD = voice.MT_CMDS
OFF_REFUSAL, MUTED_REFUSAL, NOMEM_REFUSAL = 1, 4, 5
DUCK_Q8 = 25 * 256 // 100  # 64
BIG, SMALL = 1440000, 480000
MT_RATE = voice.MT_RATE


def result(ok, name, detail=""):
    print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
    (passed if ok else failed).append(name)
    return ok


def info(text):
    print(f"INFO {text}", flush=True)


def finish():
    print(f"{len(passed)} passed, {len(failed)} failed", flush=True)
    sys.exit(1 if failed or not passed else 0)


def st(m):
    return voice.mt_state(m["state"])


def freed(m):
    """IDLE with no buffer"""
    return m["state"] == IDLE and m["heap_bytes"] == 0 and m["buf_addr"] == 0


def freed_text(m):
    return f"state {st(m)}, heap_bytes {m['heap_bytes']}, buf_addr {m['buf_addr']:08x}"


class Mic(wp.Console):
    def __init__(self, pad):
        super().__init__(pad)
        self.mt = voice.mt_addr()

    def m(self):
        m = voice.read_mt(self.x)
        if not m:
            raise SystemExit("XWMT block unreadable")
        return m

    def set_mt(self, name, v):
        self.x.setu32(self.mt + 4 * voice.MT_FIELDS.index(name), v & 0xFFFFFFFF)

    def cmd(self, name, pad=None):
        if pad is not None:
            self.set_mt("cmd_pad", pad)
        self.set_mt("cmd", CMD[name])

    def wait(self, pred, secs, step=0.05):
        """the XWMT block once pred(m) holds, or the last read after `secs`"""
        until = time.time() + secs
        while True:
            m = self.m()
            if pred(m) or time.time() >= until:
                return m
            time.sleep(step)

    def state(self, s, secs):
        return self.wait(lambda m: m["state"] == s, secs)

    def hold_panel(self):
        """Share held long enough to show the panel (it stays held), and past the mute tap's
        400 ms: letting go of Share before that toggles the mute even with the panel up"""
        self.inject(SHARE)
        time.sleep(0.6)

    def release(self):
        self.inject(0)
        time.sleep(wp.SETTLE_S)

    def discard(self):
        """whatever there is, gone (xbdm cancel), and the game back"""
        self.cmd("cancel")
        m = self.wait(lambda m: m["state"] == IDLE and m["duck_now"] == 256, 2)
        return m


def ask(hands, text):
    if not hands:
        print(f"TODO the owner check: {text}", flush=True)
        return None
    while True:
        a = input(f"OWNER CHECK: {text}\n  y / n? ").strip().lower()
        if a in ("y", "n"):
            return result(a == "y", "the owner: " + text.split(":")[0])


def to_mictest(c):
    """Share held, the cursor walked to MIC TEST; the cursor"""
    c.hold_panel()
    wp.walk_to(c, wp.MICTEST)
    return c.panel()["cursor"]


def check_buffer(m, what):
    """the buffer of a recording that just started: 1.44 MB and 30 s, or the 10 s fallback"""
    big = m["heap_bytes"] == BIG and m["max_ms"] == min(m["rec_ms"], 30000)
    small = m["heap_bytes"] == SMALL and m["max_ms"] == min(m["rec_ms"], 10000)
    result(big or small, what, f"heap_bytes {m['heap_bytes']}, auto-stop {m['max_ms']} ms, "
           f"allocation took {m['alloc_us']} us, fallbacks {m['fallback']}, failed "
           f"allocations {m['alloc_fails']}")
    if small:
        info("the 1.44 MB allocation failed: recordings stop at 10 s (the fallback works; "
             "note which title was running)")


def check_panel_run(c):
    """3: the whole thing through the panel"""
    m0 = c.m()
    c.set_mt("rec_ms", 30000)
    cur = to_mictest(c)
    if not result(cur == wp.MICTEST, "Share + Down: cursor on MIC TEST", f"cursor {cur}"):
        c.release()
        return
    c.press(RIGHT)
    m = c.state(RECORDING, 0.5)
    if not result(m["state"] == RECORDING and m["starts"] == m0["starts"] + 1,
                  "Right on MIC TEST: RECORDING", f"state {st(m)}, refused "
                  f"{m['refused'] - m0['refused']} (last {m['last_refusal']})"):
        c.release()
        return
    check_buffer(m, "a buffer for the recording")
    c.release()  # Share let go mid-recording
    time.sleep(1.3)
    m = c.m()
    result(m["state"] == RECORDING, "Share let go: still recording", f"state {st(m)}")
    result(m["secs"] >= 1 and m["clip_ms"] >= 1000 and m["secs"] == m["clip_ms"] // 1000,
           "the seconds count up", f"secs {m['secs']}, clip_ms {m['clip_ms']}, pct {m['pct']}")
    try:
        pv = hud.read_block(c.x, xw_block(hud.MAGIC))["pub_visible"]
        result(pv == 1, "panel hidden while recording: the bar shows the toast",
               f"pub_visible {pv} (1 toast)")
    except SystemExit as e:
        info(f"hud block not readable ({e}): toast not checked")
    c.hold_panel()
    cur = c.panel()["cursor"]
    result(cur == wp.MICTEST, "Share again while recording: the cursor is on MIC TEST",
           f"cursor {cur}")
    c.press(RIGHT)  # stop: it plays
    m = c.state(PLAYING, 0.5)
    result(m["state"] == PLAYING and m["plays"] == m0["plays"] + 1 and m["clip_ms"] >= 1000,
           "Right while recording: it stops and plays", f"state {st(m)}, clip {m['clip_ms']} ms")
    m = c.wait(lambda m: m["duck_now"] <= DUCK_Q8 or m["state"] != PLAYING, 1.0)
    result(m["duck_now"] == DUCK_Q8, "game ducked to 25 % while playing",
           f"duck_now {m['duck_now']} (Q8, want {DUCK_Q8})")
    c.press(RIGHT)  # pause
    m = c.state(PAUSED, 0.5)
    result(m["state"] == PAUSED and m["pauses"] == m0["pauses"] + 1, "Right while playing: PAUSED",
           f"state {st(m)}")
    m = c.wait(lambda m: m["duck_now"] == 256, 1.0)
    result(m["duck_now"] == 256, "PAUSED: the game back at 100 %", f"duck_now {m['duck_now']}")
    p1 = m["played"]
    time.sleep(0.5)
    m = c.m()
    result(m["state"] == PAUSED and m["played"] == p1 and 0 < p1 < m["recorded"],
           "PAUSED: the position holds", f"played {p1} -> {m['played']} of {m['recorded']}")
    c.press(RIGHT)  # resume
    m = c.state(PLAYING, 0.5)
    time.sleep(0.2)
    m = c.m()
    result(m["state"] in (PLAYING, READY) and m["played"] > p1 and m["pauses"] == m0["pauses"] + 1,
           "Right while paused: PLAYING on from there", f"state {st(m)}, played {p1} -> "
           f"{m['played']}")
    c.release()  # Share let go while it plays
    m = c.m()
    result(m["state"] in (PLAYING, READY), "Share let go while playing: it carries on",
           f"state {st(m)}")
    m = c.state(READY, 3)
    result(m["state"] == READY and m["done"] == m0["done"] + 1 and m["played"] == m["recorded"],
           "it plays to the end: READY, all of it played",
           f"state {st(m)}, done +{m['done'] - m0['done']}, played {m['played']} of "
           f"{m['recorded']}")
    result(m["play_peak"] <= 32767, "playback never past full scale",
           f"play_peak {m['play_peak']}, recorded peak {m['rec_peak']}, rms {m['rec_rms']}")
    result(m["heap_bytes"] > 0 and m["secs"] >= 1 and m["clip_ms"] > 0,
           "READY: the clip is held", f"secs {m['secs']}, heap_bytes {m['heap_bytes']}")
    m = c.wait(lambda m: m["duck_now"] == 256, 1.0)
    result(m["duck_now"] == 256, "READY: the game back at 100 %", f"duck_now {m['duck_now']}")
    allocs = m["allocs"]
    c.hold_panel()
    wp.walk_to(c, wp.MICTEST)
    c.press(RIGHT)  # play again
    m = c.state(PLAYING, 0.5)
    result(m["state"] == PLAYING and m["plays"] == m0["plays"] + 2 and m["allocs"] == allocs,
           "Right on a held clip: it plays again (no new recording, no new allocation)",
           f"state {st(m)}, plays +{m['plays'] - m0['plays']}, allocs +{m['allocs'] - allocs}")
    m = c.state(READY, 3)
    result(m["state"] == READY and m["done"] == m0["done"] + 2, "played again to the end: READY",
           f"state {st(m)}")
    c.release()  # the panel closes with the clip held
    m = c.wait(freed, 0.5)
    result(freed(m) and m["panel_closes"] == m0["panel_closes"] + 1,
           "Share let go with the clip held: deleted, memory freed",
           freed_text(m) + f", panel_closes +{m['panel_closes'] - m0['panel_closes']}")


def check_cancels(c, pad):
    """4: Left while recording, playing, paused"""
    c.set_mt("rec_ms", 3000)
    m0 = c.m()
    c.cmd("start", pad)
    c.state(RECORDING, 0.5)
    time.sleep(0.5)
    c.cmd("cancel")
    m = c.wait(freed, 0.5)
    result(freed(m) and m["cancels"] == m0["cancels"] + 1 and m["duck_now"] == 256,
           "Left while recording: discarded, freed, game not ducked",
           freed_text(m) + f", cancels +{m['cancels'] - m0['cancels']}, duck_now {m['duck_now']}")
    c.cmd("start", pad)
    c.state(RECORDING, 0.5)
    time.sleep(1.0)
    c.cmd("stop")
    m = c.state(PLAYING, 0.5)
    if result(m["state"] == PLAYING, "stop after 1 s: PLAYING", f"state {st(m)}"):
        time.sleep(0.2)
        c.cmd("cancel")
        m = c.wait(freed, 0.5)
        result(freed(m), "Left while playing: faded out, freed within 0.5 s", freed_text(m))
        m = c.wait(lambda m: m["duck_now"] == 256, 1.0)
        result(m["duck_now"] == 256, "the game back at 100 % after it", f"duck_now {m['duck_now']}")
    c.discard()
    c.cmd("start", pad)
    c.state(RECORDING, 0.5)
    time.sleep(1.0)
    c.cmd("stop")
    c.state(PLAYING, 0.5)
    time.sleep(0.2)
    c.cmd("pause")
    m = c.state(PAUSED, 0.5)
    result(m["state"] == PAUSED, "xbdm pause: PAUSED", f"state {st(m)}")
    c.cmd("resume")
    m = c.state(PLAYING, 0.5)
    result(m["state"] in (PLAYING, READY), "xbdm resume: PLAYING", f"state {st(m)}")
    c.cmd("pause")
    c.state(PAUSED, 0.5)
    c.cmd("cancel")
    m = c.wait(freed, 0.3)
    result(freed(m), "Left while paused: discarded, freed", freed_text(m))
    c.discard()


def check_autostop(c, pad):
    """5: the auto-stop, too short"""
    c.set_mt("rec_ms", 1000)
    m0 = c.m()
    c.cmd("start", pad)
    m = c.state(PLAYING, 2.5)
    result(m["state"] in (PLAYING, READY) and m["recorded"] == m["want"] == MT_RATE and
           m["plays"] == m0["plays"] + 1, "auto-stop after 1 s: it plays by itself",
           f"state {st(m)}, recorded {m['recorded']} of {m['want']}, max_ms {m['max_ms']}")
    m = c.state(READY, 2.5)
    result(m["state"] == READY and m["played"] == m["recorded"], "then READY, all of it played",
           f"state {st(m)}, played {m['played']}")
    c.cmd("play")
    m = c.state(PLAYING, 0.5)
    result(m["state"] in (PLAYING, READY) and m["plays"] == m0["plays"] + 2,
           "xbdm play: it plays again", f"plays +{m['plays'] - m0['plays']}")
    c.state(READY, 2.5)
    c.cmd("closed")
    m = c.wait(freed, 0.5)
    result(freed(m), "xbdm closed (the panel closing) with the clip held: freed", freed_text(m))
    c.set_mt("rec_ms", 3000)
    m0 = c.m()
    c.cmd("start", pad)
    c.state(RECORDING, 0.5)
    time.sleep(0.2)
    c.cmd("stop")
    m = c.wait(freed, 0.5)
    result(freed(m) and m["plays"] == m0["plays"] and m["discards"] > m0["discards"],
           "stop after 0.2 s: too short, discarded (not played), freed",
           freed_text(m) + f", plays +{m['plays'] - m0['plays']}")


def check_mute(c, pad):
    """6: a mute ends the recording"""
    m0 = c.m()
    c.cmd("start", pad)
    c.state(RECORDING, 0.5)
    time.sleep(0.8)
    c.tap()  # mute
    m = c.wait(lambda m: m["state"] != RECORDING, 0.5)
    result(m["state"] in (PLAYING, READY) and m["mute_stops"] == m0["mute_stops"] + 1,
           "Share tap (mute) while recording: it stops and plays",
           f"state {st(m)}, mute_stops +{m['mute_stops'] - m0['mute_stops']}")
    c.state(READY, 2.5)  # a start while it plays would be refused as busy, not as muted
    m0 = c.m()
    c.cmd("start", pad)
    time.sleep(0.2)
    m = c.m()
    result(m["state"] != RECORDING and m["refused"] == m0["refused"] + 1 and
           m["last_refusal"] == MUTED_REFUSAL, "muted: a start is refused",
           f"state {st(m)}, last refusal {m['last_refusal']}")
    c.discard()
    if c.muted():
        c.tap()
    result(not c.muted(), "unmuted again")


def check_hidden(c, pad):
    """7: a clip that ends while the panel is hidden; the panel closing while PAUSED"""
    c.set_mt("rec_ms", 1500)
    cur = to_mictest(c)
    c.press(RIGHT)
    m = c.state(RECORDING, 0.5)
    c.release()
    if not result(cur == wp.MICTEST and m["state"] == RECORDING,
                  "recording from the panel, Share let go", f"state {st(m)}"):
        c.discard()
        return
    m = c.state(READY, 5)
    result(m["state"] == READY, "it finishes and plays while the panel is hidden: READY",
           f"state {st(m)}")
    time.sleep(1.0)
    m = c.m()
    result(m["state"] == READY and m["heap_bytes"] > 0, "still held a second later",
           f"state {st(m)}, heap_bytes {m['heap_bytes']}")
    c.hold_panel()
    m = c.m()
    result(m["state"] == READY, "the panel opens: the clip is there", f"state {st(m)}")
    c.release()
    m = c.wait(freed, 0.5)
    result(freed(m), "the panel closes: deleted, freed", freed_text(m))
    c.set_mt("rec_ms", 3000)
    to_mictest(c)
    c.press(RIGHT)  # record
    time.sleep(1.0)
    c.press(RIGHT)  # stop and play
    time.sleep(0.3)
    c.press(RIGHT)  # pause
    m = c.state(PAUSED, 0.5)
    c.release()
    m2 = c.wait(freed, 0.5)
    result(m["state"] == PAUSED and freed(m2), "the panel closes while PAUSED: deleted, freed",
           f"before {st(m)}; " + freed_text(m2))
    c.discard()


def check_wav(c, pad, path):
    """8: a clip saved"""
    c.set_mt("rec_ms", 2000)
    c.cmd("start", pad)
    m = c.state(READY, 6)
    if not result(m["state"] == READY, "a 2 s clip held", f"state {st(m)}"):
        c.discard()
        return
    try:
        voice.save_mt(path)
        with wave.open(path) as f:
            n, rate, ch = f.getnframes(), f.getframerate(), f.getnchannels()
        result(rate == MT_RATE and ch == 1 and n == m["recorded"], "the clip saved as a WAV",
               f"{path}: {n / rate:.2f} s, {rate} Hz, {ch} channel(s)")
    except SystemExit as e:
        result(False, "the clip saved as a WAV", str(e))
    m = c.discard()
    result(freed(m), "then discarded, freed", freed_text(m))
    if "keep" not in m:
        info("XWMT version 2: held recordings and keep need version 3")
        return
    # 8b (version 3): voice.py mictest record, never played; keep against a panel close
    m0 = c.m()
    try:
        voice.record_mt(2, path, pad)
        with wave.open(path) as f:
            n = f.getnframes()
        m = c.m()
        result(n == 2 * MT_RATE and m["plays"] == m0["plays"] and freed(m),
               "voice.py mictest record 2 s: saved, never played, then discarded",
               f"{n} frames, plays {m0['plays']} -> {m['plays']}, " + freed_text(m))
    except SystemExit as e:
        result(False, "voice.py mictest record 2 s", str(e))
    c.set_mt("rec_ms", 2000)
    c.cmd("start-hold", pad)
    m = c.state(READY, 6)
    c.set_mt("keep", 30)
    time.sleep(wp.SETTLE_S)
    k0 = c.m()["kept"]
    c.cmd("closed")
    time.sleep(wp.SETTLE_S)
    m2 = c.m()
    result(m["state"] == READY and m["plays"] == m0["plays"] and m2["state"] == READY and
           m2["kept"] == k0 + 1, "start-hold: READY unplayed; keep 30: a panel close leaves it",
           f"{st(m)} -> {st(m2)}, kept {k0} -> {m2['kept']}")
    m = c.discard()
    result(freed(m), "then discarded, freed", freed_text(m))


def check_gate_on_playback(c, pad):
    """8c (version 3): one clip, the noise gate tried on it while it plays: Up from MIC TEST
    is NOISE GATE, a step there keeps the playback going and the playback follows it"""
    if "keep" not in c.m():
        return
    c.set_mt("rec_ms", 3000)
    c.cmd("start-hold", pad)
    m = c.state(READY, 6)
    cur = to_mictest(c)
    c.press(RIGHT)
    m = c.state(PLAYING, 0.5)
    c.press(UP)
    gate_cur = c.panel()["cursor"]
    g0 = c.diag()["applied_gate"]
    c.press(RIGHT if g0 != wp.G_4 else LEFT)
    time.sleep(wp.SETTLE_S)
    d, m2 = c.diag(), c.m()
    result(cur == wp.MICTEST and gate_cur == wp.GATE and m["state"] == PLAYING and
           m2["state"] == PLAYING and d["applied_gate"] != g0 and
           m2["rec_gate"] == d["applied_gate"],
           "while the clip plays: Up is NOISE GATE, a step there keeps it playing, the playback "
           "uses the new step", f"cursor {cur} -> {gate_cur}, gate {g0} -> {d['applied_gate']}, "
           f"state {st(m)} -> {st(m2)}, playback gate {m2['rec_gate']}")
    c.press(LEFT if g0 != wp.G_4 else RIGHT)
    time.sleep(wp.SETTLE_S)
    result(c.diag()["applied_gate"] == g0, "noise gate put back", str(c.diag()["applied_gate"]))
    c.release()
    m = c.discard()
    result(freed(m), "then discarded, freed", freed_text(m))


def check_fallback(c, pad):
    """9: the allocation fallbacks (XW_DEV builds can force them)"""
    s = xw.read_status(c.x, xw.status_addr())
    if s and s["audio"] and s["audio"]["release"]:
        info("release build: the forced 10 s fallback and out-of-memory checks need XW_DEV")
        return
    c.set_mt("rec_ms", 30000)
    m0 = c.m()
    c.cmd("start-small", pad)
    m = c.state(RECORDING, 0.5)
    result(m["state"] == RECORDING and m["heap_bytes"] == SMALL and m["max_ms"] == 10000 and
           m["fallback"] == m0["fallback"] + 1, "forced fallback: 480 KB, auto-stop 10 s",
           f"state {st(m)}, heap_bytes {m['heap_bytes']}, max_ms {m['max_ms']}")
    m = c.discard()
    result(freed(m) and m["max_ms"] == 10000, "discarded, freed; the panel is told 10 s",
           freed_text(m) + f", max_ms {m['max_ms']}")
    m0 = c.m()
    c.cmd("start-nomem", pad)
    m = c.wait(lambda m: m["refused"] > m0["refused"], 0.5)
    result(freed(m) and m["last_refusal"] == NOMEM_REFUSAL, "forced out of memory: refused",
           f"last refusal {m['last_refusal']}, " + freed_text(m))
    c.cmd("start", pad)
    m = c.state(RECORDING, 0.5)
    check_buffer(m, "a normal start after that: the buffer again")
    c.discard()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pad", type=int, default=0, choices=range(4))
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--wav", default=os.path.join(TOOLS, "out", "mictest.wav"))
    a = ap.parse_args()

    # 1. the build
    m = voice.read_mt(wp.Xbdm())
    if not result(m is not None, f"XWMT version {voice.MT_VERSION} (mic test)", "found" if m else
                  "missing: install a build with MICTEST v2 (xbox360/out/xw.xex)"):
        return finish()
    c = Mic(a.pad)
    d = xw.read_diag(c.x)
    result(d is not None, f"XWDG version {xw.DIAG_VERSION}", "read" if d else "missing")
    p = c.panel()
    result(p["version"] == wp.PN_VERSION, f"XWPN version {wp.PN_VERSION}",
           f"version {p['version']}")
    result(m["rec_ms"] == 30000 and m["duck_pct"] == 25 and m["enable"] == 1,
           "defaults: auto-stop 30 s, game at 25 % while playing, on",
           f"rec_ms {m['rec_ms']}, duck {m['duck_pct']} %, enable {m['enable']}")
    result(freed(m) or m["state"] != IDLE, "idle means nothing allocated", freed_text(m))
    info(f"since boot: {m['allocs']} allocations ({m['alloc_fails']} failed, {m['fallback']} "
         f"fell back to 10 s), the last of {m['buf_bytes']} bytes in {m['alloc_us']} us")
    if d:
        info(f"mic volume {d['applied_mic_gain']} %, gate {d['applied_gate']}, DC removal "
             f"{'on' if d['mic_dc'] else 'off'}, meter {d['mic_vu']}/10")
    if os.environ.get("ACCEPT_READ_ONLY") == "1":
        voice.print_mt(m)
        info("read-only: stopping before any write")
        return finish()
    if p["visible"] or p["share_down"]:
        c.release()
    s0 = dict(enable=m["enable"], muted=c.muted())
    if s0["muted"]:
        c.tap()
    c.discard()
    try:
        # 2. switched off
        c.set_mt("enable", 0)
        time.sleep(wp.SETTLE_S)
        m0 = c.m()
        c.cmd("start", a.pad)
        m = c.wait(lambda m: m["refused"] > m0["refused"], 0.5)
        result(m["state"] == IDLE and m["last_refusal"] == OFF_REFUSAL,
               "enable 0: a start is refused (switched off)",
               f"state {st(m)}, last refusal {m['last_refusal']}")
        cur = to_mictest(c)
        result(cur != wp.MICTEST, "enable 0: MIC TEST cannot be reached", f"cursor {cur}")
        c.release()
        c.set_mt("enable", 1)
        time.sleep(wp.SETTLE_S)

        check_panel_run(c)          # 3
        check_cancels(c, a.pad)     # 4
        check_autostop(c, a.pad)    # 5
        check_mute(c, a.pad)        # 6
        check_hidden(c, a.pad)      # 7
        check_wav(c, a.pad, a.wav)  # 8
        check_gate_on_playback(c, a.pad)  # 8c
        check_fallback(c, a.pad)    # 9
        c.set_mt("rec_ms", 30000)
        ask(a.hands, "J1 Hold Share, go to MIC TEST, press Right and talk for as long as you "
            "like, press Right to stop and hear it, Right to pause, Right to carry on, then let "
            "it finish and press Right to hear it again; let go of Share and open the panel "
            "again: the clip is gone")
    finally:
        # 10. put back
        c.release()
        c.discard()
        c.set_mt("rec_ms", 30000)
        c.set_mt("duck_pct", 25)
        c.set_mt("enable", s0["enable"])
        if c.muted() != s0["muted"]:
            c.tap()
        m = c.wait(lambda m: m["max_ms"] in (10000, 30000), 0.5)
        result(m["rec_ms"] == 30000 and freed(m) and m["duck_now"] == 256 and
               m["enable"] == s0["enable"] and c.muted() == s0["muted"], "settings put back",
               f"rec_ms {m['rec_ms']}, {freed_text(m)}, duck_now {m['duck_now']}, enable "
               f"{m['enable']}")
    finish()


if __name__ == "__main__":
    main()
