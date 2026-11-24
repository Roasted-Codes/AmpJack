#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Xbox 360 headset bridge: the PC side of xw.xex's PC bridge (xbox360/bridge_360.c).

Linux with PipeWire or PulseAudio (it uses pactl, parec and pacat, which both provide):

  python3 xbox-bridge.py 10.0.0.13             four sound devices for any app
  python3 xbox-bridge.py 10.0.0.13 --bridge    ... plus two that wire Discord up in one go

It makes these devices while it runs and removes them when it stops (Ctrl+C):
  Xbox Headset                  (output)  what plays here goes into the headset
  Xbox Chat (to Xbox players)   (output)  what plays here goes to the Xbox players
  Xbox Headset Mic              (input)   the headset's microphone
  Xbox Players                  (input)   what the Xbox players say
  with --bridge, also:
  Xbox Bridge                   (output)  into the headset AND to the Xbox players
  Xbox Bridge Mic               (input)   the headset's microphone AND the Xbox players

Test mode, no sound devices needed (any OS, used from the Mac to check the console):

  python3 xbox-bridge.py 10.0.0.13 --test [--secs 10] [--tone 3|4|34|none] [--dir DIR]

sends a tone to the headset (440 Hz, stream 3) and/or to the Xbox chat (660 Hz, stream 4),
records the headset mic and the Xbox players to WAV files and prints what arrived, what was
lost, and the console's own counters.

Python 3 standard library only. The wire format is bridge_360.c's (keep the two in step).
"""
import argparse
import array
import math
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

PORT = 3670
MAGIC = b"XBRG"
VERSION = 1
HELLO, STATUS, AUDIO, BYE = 1, 2, 3, 4
HDR = struct.Struct(">4sBBBBIIIHH")  # magic version type stream channels rate seq ts frames flags
assert HDR.size == 24
MAX_SAMPLE_BYTES = 1440              # PC -> console, per packet (one Ethernet frame)
HELLO_S = 1.0
PEER_TIMEOUT_S = 3.0                 # the console's (bridge_360.c PEER_TIMEOUT_MS)
STATUS_FIELDS = ("enable want3_rate want3_ch want4_rate want4_ch headset "
                 "s3_packets s3_lost s3_late s3_underruns s3_restarts s3_fill_ms s3_trim_ppm "
                 "s4_packets s4_lost s4_late s4_underruns s4_restarts s4_fill_ms s4_trim_ppm "
                 "s1_packets s1_skipped s2_packets s2_skipped "
                 "pass_us_max mic_us_max ear_us_max net_us_max").split()
assert len(STATUS_FIELDS) == 28
NAMES = {1: "headset mic", 2: "Xbox players", 3: "to headset", 4: "to Xbox chat"}


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


# ---- the link to the console -----------------------------------------------------------------

class StreamIn:
    """counts what arrives of stream 1 or 2 and hands it on in order"""

    def __init__(self, stream):
        self.stream = stream
        self.packets = self.lost = self.late = self.frames = 0
        self.next_seq = None
        self.rate = 0
        self.peak = 0
        self.sink = None  # sink.push(ts, rate, be_bytes)

    def take(self, seq, ts, rate, data):
        self.packets += 1
        self.frames += len(data) // 2
        self.rate = rate
        if self.next_seq is None or abs(signed((seq - self.next_seq) & 0xFFFFFFFF)) > 1000:
            self.next_seq = seq + 1
        else:
            d = signed((seq - self.next_seq) & 0xFFFFFFFF)
            if d >= 0:
                self.lost += d
                self.next_seq = seq + 1
            else:
                self.late += 1
                if self.lost:
                    self.lost -= 1
        if self.sink:
            self.sink.push(ts, rate, data)


class Link:
    """the UDP session: hellos once a second, the console's STATUS answers, streams 1 and 2 in,
    streams 3 and 4 out"""

    def __init__(self, host, port=PORT, want=3, log=print):
        self.addr = (socket.gethostbyname(host), port)
        self.want = want
        self.log = log
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.s.bind(("", 0))
        self.s.settimeout(0.2)
        self.status = None
        self.status0 = None  # the first answer: the console's counters run since it started
        self.status_at = 0.0
        self.statuses = 0
        self.ins = {1: StreamIn(1), 2: StreamIn(2)}
        self.out_seq = {3: 0, 4: 0}
        self.out_ts = {3: random.getrandbits(32), 4: random.getrandbits(32)}
        self.out_packets = {3: 0, 4: 0}
        self.bad = 0
        self.lock = threading.Lock()
        self.running = True
        self.on_status = None  # called with the new status dict
        self.threads = [threading.Thread(target=self._rx, daemon=True),
                        threading.Thread(target=self._hello, daemon=True)]
        for t in self.threads:
            t.start()

    def connected(self):
        return self.status is not None and time.monotonic() - self.status_at < PEER_TIMEOUT_S

    def _send(self, b):
        try:
            self.s.sendto(b, self.addr)
        except OSError as e:
            self.log(f"send failed: {e}")

    def _hello(self):
        while self.running:
            self._send(HDR.pack(MAGIC, VERSION, HELLO, 0, 0, 0, 0, 0, 0, 0)
                       + struct.pack(">I", self.want))
            time.sleep(HELLO_S)

    def _rx(self):
        while self.running:
            try:
                b, src = self.s.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                if not self.running:
                    return
                continue
            if src[0] != self.addr[0] or len(b) < HDR.size:
                continue
            magic, ver, typ, stream, ch, rate, seq, ts, frames, _ = HDR.unpack_from(b)
            if magic != MAGIC or ver != VERSION:
                self.bad += 1
                continue
            if typ == STATUS:
                n = (len(b) - HDR.size) // 4
                w = struct.unpack_from(f">{n}I", b, HDR.size)
                st = dict(zip(STATUS_FIELDS, w))
                for k in ("s3_trim_ppm", "s4_trim_ppm"):
                    if k in st:
                        st[k] = signed(st[k])
                if self.status0 is None:
                    self.status0 = st
                self.status = st
                self.status_at = time.monotonic()
                self.statuses += 1
                if self.on_status:
                    self.on_status(st)
            elif typ == AUDIO and stream in (1, 2) and ch == 1 and len(b) == HDR.size + 2 * frames:
                self.ins[stream].take(seq, ts, rate, b[HDR.size:])
            else:
                self.bad += 1

    def send_audio(self, stream, rate, ch, pcm_be):
        """frames of ch-channel s16 big-endian at rate, cut into packets"""
        step = MAX_SAMPLE_BYTES // (2 * ch) * 2 * ch
        with self.lock:
            for off in range(0, len(pcm_be), step):
                part = pcm_be[off:off + step]
                n = len(part) // (2 * ch)
                if not n:
                    continue
                self._send(HDR.pack(MAGIC, VERSION, AUDIO, stream, ch, rate,
                                    self.out_seq[stream] & 0xFFFFFFFF,
                                    self.out_ts[stream] & 0xFFFFFFFF, n, 0) + part[:n * 2 * ch])
                self.out_seq[stream] += 1
                self.out_ts[stream] += n
                self.out_packets[stream] += 1

    def close(self):
        self.running = False
        try:
            self._send(HDR.pack(MAGIC, VERSION, BYE, 0, 0, 0, 0, 0, 0, 0))
        finally:
            time.sleep(0.05)
            self.s.close()


def wanted(st, stream):
    """(rate, channels) the console asks for on stream 3 or 4"""
    if not st:
        return (48000, 2) if stream == 3 else (24000, 1)
    return (st[f"want{stream}_rate"], st[f"want{stream}_ch"])


# ---- sources: what goes to the console (streams 3, 4) ------------------------------------------

def packet_frames(rate, ch):
    return min(rate // 100, MAX_SAMPLE_BYTES // (2 * ch))  # 10 ms, or what fits a frame


class ToneSource:
    """a sine at -20 dBFS, paced by the clock (test mode)"""

    def __init__(self, link, stream, hz):
        self.link, self.stream, self.hz = link, stream, hz
        self.phase = 0.0
        self.running = True
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        start = time.monotonic()
        sent = 0
        fmt = None
        while self.running:
            rate, ch = wanted(self.link.status, self.stream)
            if (rate, ch) != fmt:
                fmt = (rate, ch)
                start, sent = time.monotonic(), 0
            n = packet_frames(rate, ch)
            due = int((time.monotonic() - start) * rate)
            if due - sent > rate // 5:  # held up 200 ms: skip ahead
                sent = due - n
            while sent + n <= due:
                a = array.array("h")
                for i in range(n):
                    v = int(3277 * math.sin(self.phase))
                    self.phase += 2 * math.pi * self.hz / rate
                    a.extend([v] * ch)
                if sys.byteorder == "little":
                    a.byteswap()
                self.link.send_audio(self.stream, rate, ch, a.tobytes())
                sent += n
            self.phase %= 2 * math.pi
            time.sleep(0.004)

    def stop(self):
        self.running = False


def to_mono(be_stereo):
    """s16 big-endian stereo -> mono, the average of the two"""
    a = array.array("h", be_stereo)
    if sys.byteorder == "little":
        a.byteswap()
    m = array.array("h", [(a[i] + a[i + 1]) >> 1 for i in range(0, len(a) - 1, 2)])
    if sys.byteorder == "little":
        m.byteswap()
    return m.tobytes()


class ParecSource:
    """a sink's monitor recorded with parec in the format the console wants, forwarded as it comes
    (the sink runs on the PC's clock; the console trims for the difference)"""

    def __init__(self, link, stream, device, log=print):
        self.link, self.stream, self.device, self.log = link, stream, device, log
        self.proc = None
        self.fmt = None
        self.running = True
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _start(self, rate, ch):
        self._kill()
        self.fmt = (rate, ch)
        # always stereo: a mono stream is averaged here, as PulseAudio would. PipeWire's own
        # stereo-to-mono adds the two at 0.7 each, 3 dB louder for a voice in both channels
        self.proc = subprocess.Popen(
            ["parec", f"--device={self.device}", "--format=s16be", f"--rate={rate}",
             "--channels=2", "--latency-msec=10", "--raw",
             f"--client-name=xbox-bridge {NAMES[self.stream]}"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)

    def _kill(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None

    def _run(self):
        while self.running:
            if not self.link.connected():
                self._kill()
                time.sleep(0.2)
                continue
            rate, ch = wanted(self.link.status, self.stream)
            if (rate, ch) != self.fmt or not self.proc or self.proc.poll() is not None:
                self._start(rate, ch)
            need = packet_frames(rate, ch) * 4  # stereo frames
            buf = b""
            while len(buf) < need and self.running:
                part = self.proc.stdout.read(need - len(buf))
                if not part:
                    break
                buf += part
            if len(buf) == need:
                if ch == 1:
                    buf = to_mono(buf)
                self.link.send_audio(self.stream, rate, ch, buf)
            elif self.running:
                time.sleep(0.2)  # parec ended: started again above

    def stop(self):
        self.running = False
        self._kill()


# ---- sinks: what comes from the console (streams 1, 2) ------------------------------------------

class WavSink:
    """test mode: the stream as it arrived, gaps filled with silence, to a WAV file"""

    def __init__(self, path):
        self.path = path
        self.w = None
        self.next_ts = None
        self.rate = 0
        self.frames = 0
        self.gap_frames = 0
        self.closed = False
        self.lock = threading.Lock()

    def push(self, ts, rate, data):
        with self.lock:
            if self.closed:
                return
            if self.w is None:
                self.w = wave.open(self.path, "wb")
                self.w.setnchannels(1)
                self.w.setsampwidth(2)
                self.w.setframerate(rate)
                self.rate = rate
                self.next_ts = ts
            if rate != self.rate:
                return  # one rate per file
            d = signed((ts - self.next_ts) & 0xFFFFFFFF)
            if d < 0:
                return  # late
            if d > 0:
                gap = min(d, rate * 2)
                self.w.writeframes(b"\0\0" * gap)
                self.gap_frames += gap
                self.frames += gap
            a = array.array("h", data)
            if sys.byteorder == "little":
                a.byteswap()
            self.w.writeframes(a.tobytes())
            self.frames += len(a)
            self.next_ts = ts + len(a)

    def close(self):
        with self.lock:
            self.closed = True
            if self.w:
                self.w.close()


class PacedSink:
    """a stream from the console played into one or more sinks with pacat, paced by the PC's
    clock: it keeps about 60 ms queued and, when the console's clock runs fast or slow against
    the PC's, drops or repeats one sample now and then (inaudible) to stay there. Gaps are
    silence; an empty queue plays silence until 60 ms are there again."""

    TICK_S = 0.010
    TARGET_MS = 60
    BAND_MS = 25

    def __init__(self, devices, name, log=print):
        self.devices, self.name, self.log = devices, name, log
        self.procs = []
        self.rate = 0
        self.buf = bytearray()
        self.next_ts = None
        self.playing = False
        self.underruns = self.dropped = self.repeated = 0
        self.lock = threading.Lock()
        self.running = True
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _start(self, rate):
        self._kill()
        self.rate = rate
        for dev in self.devices:
            self.procs.append(subprocess.Popen(
                ["pacat", f"--device={dev}", "--format=s16be", f"--rate={rate}", "--channels=1",
                 "--latency-msec=30", "--raw", f"--client-name=xbox-bridge {self.name}"],
                stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0))

    def _kill(self):
        for p in self.procs:
            try:
                p.stdin.close()
            except OSError:
                pass
            p.terminate()
            try:
                p.wait(2)
            except subprocess.TimeoutExpired:
                p.kill()
        self.procs = []

    def push(self, ts, rate, data):
        with self.lock:
            if rate != self.rate:
                self._start(rate)
                self.buf.clear()
                self.next_ts = None
                self.playing = False
            if self.next_ts is not None:
                d = signed((ts - self.next_ts) & 0xFFFFFFFF)
                if d < 0:
                    return  # late: already played over
                if d > rate:  # a long pause or a restart: start over
                    self.buf.clear()
                    self.playing = False
                elif d > 0:
                    self.buf += b"\0\0" * d  # lost packets: silence
            self.buf += data
            self.next_ts = ts + len(data) // 2
            if len(self.buf) > 2 * rate:  # more than a second: nobody is playing it
                del self.buf[:len(self.buf) - 2 * rate // 10]

    def _write(self, chunk):
        for p in list(self.procs):
            try:
                p.stdin.write(chunk)
            except (BrokenPipeError, OSError, ValueError):
                with self.lock:
                    if self.running and p in self.procs:
                        self.log(f"{self.name}: pacat stopped; starting it again")
                        self._start(self.rate)
                return

    def _run(self):
        t_next = time.monotonic()
        while self.running:
            t_next += self.TICK_S
            delay = t_next - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                t_next = time.monotonic()  # held up: don't try to catch up
            with self.lock:
                if not self.rate or not self.procs:
                    continue
                n = int(self.rate * self.TICK_S)
                have = len(self.buf) // 2
                target = self.rate * self.TARGET_MS // 1000
                band = self.rate * self.BAND_MS // 1000
                if not self.playing and have >= target:
                    self.playing = True
                if self.playing and have < n:
                    self.playing = False
                    self.underruns += 1
                if not self.playing:
                    chunk = b"\0\0" * n
                else:
                    take = n + 1 if have > target + band else n - 1 if have < target - band else n
                    take = min(take, have)
                    chunk = bytes(self.buf[:2 * take])
                    del self.buf[:2 * take]
                    if take == n + 1:  # one sample dropped (the console's clock is fast)
                        mid = n // 2 * 2
                        chunk = chunk[:mid] + chunk[mid + 2:]
                        self.dropped += 1
                    elif take == n - 1 and take > 0:  # one repeated (it is slow)
                        chunk = chunk + chunk[-2:]
                        self.repeated += 1
                    if len(chunk) < 2 * n:
                        chunk += b"\0\0" * (n - len(chunk) // 2)
            self._write(chunk)

    def close(self):
        self.running = False
        with self.lock:
            self._kill()


# ---- the sound devices (pactl) ------------------------------------------------------------------

DEV = {
    "headset": ("xbox_headset", "Xbox Headset"),
    "chat": ("xbox_chat", "Xbox Chat (to Xbox players)"),
    "mic_feed": ("xbox_mic_feed", "Xbox bridge internal 1 (not for apps)"),
    "players_feed": ("xbox_players_feed", "Xbox bridge internal 2 (not for apps)"),
    "mic": ("xbox_headset_mic", "Xbox Headset Mic"),
    "players": ("xbox_players", "Xbox Players"),
    "bridge": ("xbox_bridge", "Xbox Bridge"),
    "bridge_feed": ("xbox_bridge_mic_feed", "Xbox bridge internal 3 (not for apps)"),
    "bridge_mic": ("xbox_bridge_mic", "Xbox Bridge Mic"),
}


def pactl(*args):
    r = subprocess.run(["pactl", *args], capture_output=True, text=True)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def props(key, desc):
    # one argument for pactl: PulseAudio and PipeWire both read the outer double quotes as the
    # module argument and the single-quoted description inside as the property's value
    return f"{key}=\"device.description='{desc}'\""


class Devices:
    def __init__(self, bridge, log=print):
        self.modules = []
        self.log = log
        self.bridge = bridge

    def _load(self, *args):
        rc, out, err = pactl("load-module", *args)
        if rc or not out.strip().isdigit():
            raise RuntimeError(f"pactl load-module {' '.join(args)}: {err or out}")
        self.modules.append(out.strip())

    def _sink(self, key, channels=2):
        name, desc = DEV[key]
        self._load("module-null-sink", f"sink_name={name}", "rate=48000", f"channels={channels}",
                   props("sink_properties", desc))

    def _source(self, key, feed_key):
        name, desc = DEV[key]
        # mono, as the feed: PipeWire otherwise makes a stereo source and records it back to
        # mono 3 dB louder
        self._load("module-remap-source", f"source_name={name}",
                   f"master={DEV[feed_key][0]}.monitor", "channels=1", "channel_map=mono",
                   "master_channel_map=mono", props("source_properties", desc))

    def _loopback(self, src_key, sink_key):
        self._load("module-loopback", f"source={DEV[src_key][0]}.monitor",
                   f"sink={DEV[sink_key][0]}", "latency_msec=20", "source_dont_move=true",
                   "sink_dont_move=true")

    def leftovers(self):
        """unloads the devices of a run that did not end cleanly (newest first)"""
        rc, out, _ = pactl("list", "short", "modules")
        if rc:
            return
        ours = {v[0] for v in DEV.values()}
        found = []
        for line in out.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3 and set(re.findall(r"=(xbox_[a-z_]+)(?:\.monitor)?\b", parts[2])) & ours:
                found.append(parts[0])
        for m in reversed(found):
            pactl("unload-module", m)
        if found:
            self.log(f"removed {len(found)} Xbox devices left over from an earlier run")

    def create(self):
        self.leftovers()
        try:
            self._sink("headset")
            self._sink("chat")
            self._sink("mic_feed", 1)
            self._sink("players_feed", 1)
            self._source("mic", "mic_feed")
            self._source("players", "players_feed")
            if self.bridge:
                self._sink("bridge")
                self._loopback("bridge", "headset")
                self._loopback("bridge", "chat")
                self._sink("bridge_feed", 1)
                self._source("bridge_mic", "bridge_feed")
        except Exception:
            self.remove()
            raise

    def remove(self):
        for m in reversed(self.modules):
            pactl("unload-module", m)
        self.modules = []


# ---- the modes ------------------------------------------------------------------------------------

def show_status(link, log):
    st = link.status
    i1, i2 = link.ins[1], link.ins[2]
    if not link.connected():
        log("  waiting for the Xbox (is it on, with xw.xex? is the address right?)")
        return
    hs = "streaming" if st["headset"] else ("NOT streaming (press the controller's Xbox button, "
                                            "plug the headset in)")
    log(f"  Xbox: connected, headset {hs}")
    log(f"  from the Xbox: headset mic {i1.packets} packets ({i1.lost} lost), "
        f"Xbox players {i2.packets} packets ({i2.lost} lost)")
    st0 = link.status0 or st

    def got(k, f):
        return st[f"s{k}_{f}"] - st0[f"s{k}_{f}"]  # this session's, not since the Xbox started

    log(f"  to the Xbox:   headset {link.out_packets[3]} packets (it got {got(3, 'packets')}, "
        f"lost {got(3, 'lost')}, underruns {got(3, 'underruns')}), "
        f"chat {link.out_packets[4]} packets (it got {got(4, 'packets')}, lost {got(4, 'lost')}, "
        f"underruns {got(4, 'underruns')})")


def run_test(args, log=print):
    """the --test mode; returns a dict of results (accept/bridge.py uses it)"""
    d = args.dir or tempfile.mkdtemp(prefix="xbox-bridge-")
    os.makedirs(d, exist_ok=True)
    link = Link(args.host, args.port, log=log)
    wavs = {1: WavSink(os.path.join(d, "headset-mic.wav")),
            2: WavSink(os.path.join(d, "xbox-players.wav"))}
    link.ins[1].sink, link.ins[2].sink = wavs[1], wavs[2]
    t0 = time.monotonic()
    while not link.connected() and time.monotonic() - t0 < 3:
        time.sleep(0.05)
    res = {"connected": link.connected(), "connect_s": time.monotonic() - t0, "dir": d}
    if not res["connected"]:
        log(f"no answer from {args.host}:{args.port} in 3 s (xw.xex with the bridge running? "
            f"bridge.py status on the Mac says)")
        link.close()
        return res
    log(f"connected in {res['connect_s'] * 1000:.0f} ms; the Xbox wants: headset "
        f"{wanted(link.status, 3)}, chat {wanted(link.status, 4)} (rate, channels)")
    first = dict(link.status)
    tones = []
    if "3" in args.tone:
        tones.append(ToneSource(link, 3, 440))
    if "4" in args.tone:
        tones.append(ToneSource(link, 4, 660))
    end = time.monotonic() + args.secs
    next_show = time.monotonic() + 5
    while time.monotonic() < end:
        time.sleep(0.1)
        if time.monotonic() >= next_show:
            show_status(link, log)
            next_show += 5
    during = dict(link.status or {})  # while the tones still play (the fill, the trim)
    for t in tones:
        t.stop()
    time.sleep(0.5)  # the last packets, and a STATUS after them
    last = dict(link.status or {})
    for w in wavs.values():
        w.close()
    link.close()
    res.update(first=first, last=last, during=during, sent={3: link.out_packets[3], 4: link.out_packets[4]},
               ins={k: vars(v).copy() for k, v in link.ins.items()},
               wav={k: (w.path, w.frames, w.gap_frames, w.rate) for k, w in wavs.items()},
               statuses=link.statuses, bad=link.bad)
    for k in (1, 2):
        i = link.ins[k]
        path, frames, gaps, rate = res["wav"][k]
        log(f"stream {k} ({NAMES[k]}): {i.packets} packets, {i.lost} lost, {i.late} late; "
            + (f"{frames / rate:.1f} s at {rate} Hz in {path} ({gaps} frames of gap filled)"
               if rate else "nothing arrived (no headset streaming?)"))
    for k in (3, 4):
        if str(k) in args.tone:
            got = last.get(f"s{k}_packets", 0) - first.get(f"s{k}_packets", 0)
            log(f"stream {k} ({NAMES[k]}): sent {link.out_packets[k]}, the Xbox counted {got}; "
                f"lost {last.get(f's{k}_lost', 0) - first.get(f's{k}_lost', 0)}, underruns "
                f"{last.get(f's{k}_underruns', 0) - first.get(f's{k}_underruns', 0)}, fill "
                f"{during.get(f's{k}_fill_ms', 0)} ms, trim {during.get(f's{k}_trim_ppm', 0)} ppm")
    log(f"Xbox timing (max): audio work per pass {last.get('pass_us_max')} us, mic call "
        f"{last.get('mic_us_max')} us, earpiece call {last.get('ear_us_max')} us, net pass "
        f"{last.get('net_us_max')} us; headset {'streaming' if last.get('headset') else 'not streaming'}")
    return res


def run_live(args, log=print):
    for tool in ("pactl", "parec", "pacat"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} not found: install pulseaudio-utils (or pipewire-pulse), "
                             f"or use --test")
    rc, _, err = pactl("info")
    if rc:
        raise SystemExit(f"no sound server answers pactl ({err}): is PipeWire or PulseAudio running?")
    devs = Devices(args.bridge, log)
    devs.create()
    link = Link(args.host, args.port, log=log)
    mic_to = [DEV["mic_feed"][0]] + ([DEV["bridge_feed"][0]] if args.bridge else [])
    players_to = [DEV["players_feed"][0]] + ([DEV["bridge_feed"][0]] if args.bridge else [])
    sinks = [PacedSink(mic_to, "headset mic", log), PacedSink(players_to, "Xbox players", log)]
    link.ins[1].sink, link.ins[2].sink = sinks
    sources = [ParecSource(link, 3, DEV["headset"][0] + ".monitor", log),
               ParecSource(link, 4, DEV["chat"][0] + ".monitor", log)]
    stop = threading.Event()

    def on_signal(*_):
        stop.set()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    log("")
    log("Sound devices are ready. In Discord: User Settings > Voice & Video.")
    if args.bridge:
        log('  Input Device:  "Xbox Bridge Mic"  (your headset mic and the Xbox players)')
        log('  Output Device: "Xbox Bridge"      (you hear it in the headset, and so do the '
            'Xbox players)')
    else:
        log('  Input Device:  "Xbox Headset Mic"  (your headset mic)')
        log('  Output Device: "Xbox Headset"      (you hear Discord in the headset)')
        log('  Also there: "Xbox Players" (an input: what the Xbox players say) and')
        log('  "Xbox Chat (to Xbox players)" (an output: what the Xbox players hear).')
        log("  For both directions at once, run it with --bridge.")
    log("Leave this window open; press Ctrl+C to stop (the devices go away).")
    log("")
    was = None
    next_show = time.monotonic() + 2
    try:
        while not stop.is_set():
            stop.wait(0.5)
            now_conn = link.connected()
            if now_conn != was:
                was = now_conn
                log("Xbox connected." if now_conn else f"Waiting for the Xbox at {args.host} ...")
                next_show = time.monotonic() + 5
            if time.monotonic() >= next_show:
                next_show = time.monotonic() + 60
                show_status(link, log)
    finally:
        for s in sources:
            s.stop()
        for s in sinks:
            s.close()
        link.close()
        devs.remove()
        log("Stopped; the Xbox devices are removed.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 1)[1])
    ap.add_argument("host", help="the Xbox's address, e.g. 10.0.0.13")
    ap.add_argument("--bridge", action="store_true",
                    help='also make "Xbox Bridge" and "Xbox Bridge Mic" for Discord')
    ap.add_argument("--test", action="store_true", help="test mode: tones out, WAV files in")
    ap.add_argument("--secs", type=float, default=10, help="test mode: how long (default 10)")
    ap.add_argument("--tone", default="34", choices=["3", "4", "34", "none"],
                    help="test mode: tone on stream 3 (headset), 4 (chat), both, or none")
    ap.add_argument("--dir", help="test mode: where the WAV files go (default: a new temp dir)")
    ap.add_argument("--port", type=int, default=PORT, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.test:
        run_test(args)
    else:
        run_live(args)


if __name__ == "__main__":
    main()
