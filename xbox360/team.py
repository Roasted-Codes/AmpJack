#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Team voice (team_360.c, docs/TEAM.md): the console's XWTV block over xbdm, and a PC peer
that speaks the same UDP protocol, so one console can be tested from a Mac.

Console (xbdm; XBOX=host[:port] required):
  team.py status                  the XWTV block: stage, socket, errors, traffic, peers, talkers
  team.py stage N [CH]            0 off, 1 socket only, 2 hello, 3 receive, 4 send; CH = channel
  team.py set NAME VALUE          any control (stage channel level mic_route test_tone dest port
                                  opts startup loop_own jb_ms hw_thread priority debug wake);
                                  dest takes a dotted address (0 = 255.255.255.255);
                                  test_tone 1 = a 440 Hz stream from the net thread, 2 = a
                                  synthetic mic (440 Hz, gated 1 s on / 1 s off) through the
                                  whole send path (needs a streaming headset)
  team.py zero                    reset the traffic and error counters (not the controls)
  team.py dump FILE.wav           1 s of the team mix as added to the earpiece (after the gain)
                                  as a WAV, and its 440 Hz content

PC peer (no xbdm; UDP 3660 on this machine):
  team.py listen [SECS]           print every HELLO / BYE and a per-sender AUDIO summary
  team.py hello [CH] [SECS]       be a peer: HELLO once a second on CH (default 1); prints what
                                  it hears
  team.py tone CH [SECS]          HELLO + a 440 Hz AUDIO stream on CH (10 ms packets, -20 dBFS)
  team.py record CH FILE.wav [SECS]  record channel CH (every talker on it, mixed) to a WAV,
                                  with loss, reorder and talk-spurt statistics
  team.py reflect [SECS]          send every console AUDIO / HELLO packet back as another
                                  sender (id with the top bit flipped, XT_F_PC): a second
                                  "console" for a one-console loopback test
  Options for the peer modes: --dest A.B.C.D (default 255.255.255.255), --port N (3660),
  --hz F (440), --dbfs D (-20), --name NAME, --id HEX.
"""
import argparse
import math
import os
import random
import socket
import struct
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ---- the wire format (team_360.c, docs/TEAM.md) ---------------------------------------------
XT_MAGIC = 0x58575456  # "XWTV"
XT_VERSION = 1
XT_PORT = 3660
HELLO, AUDIO, BYE = 1, 2, 3
F_TALKING, F_MUTED, F_PC, F_ALLCALL = 1, 2, 4, 8
HDR = struct.Struct(">IBBBBIHHI")  # magic version type channel flags sender seq samples timestamp
HELLO_PAYLOAD = struct.Struct(">16sB3x")
RATE = 16000
PACKET = 160
TYPES = {HELLO: "HELLO", AUDIO: "AUDIO", BYE: "BYE"}

# ---- the XWTV block (struct xt_status in team_360.c; all u32, in this order) ---------------
PEERS, TALKERS = 16, 4
HEAD = ("magic version stage channel level mic_route test_tone dest port opts startup loop_own "
        "jb_ms hw_thread priority debug "
        "applied_stage init_error init_detail thread net_state heartbeat now_ms sender ip mac_hi "
        "xnaddr_flags link socket opens reopens title_switches quiesce "
        "err_xnet err_wsa err_open open_step err_send send_fails err_recv recv_fails "
        "tx_hello tx_audio tx_bye tx_bytes tx_us_last tx_us_max tx_us_sum tx_calls "
        "rx_raw rx_bytes rx_hello rx_audio rx_bye rx_bad rx_own rx_other_ch rx_no_talker rx_us_max "
        "peers_known team_peers talking_mask tone_skips "
        "mix_calls mix_pad rate_out ch_out gain out_peak dump dump_addr dump_frames dump_rate").split()
PEER_FIELDS = "id ip chan name0 name1 name2 name3 age_ms rx_hello rx_audio lost late jitter_us talker".split()
TALKER_FIELDS = ("id state fill_ms target_ms jitter_us underruns overruns trim_ppm starts spurts "
                 "concealed recovered late restarts mixed_ms peak").split()
assert len(HEAD) == 73  # _Static_assert in team_360.c
NWORDS_V1 = len(HEAD) + PEERS * len(PEER_FIELDS) + TALKERS * len(TALKER_FIELDS)
# version 2: struct xt_tx_status (the send path, WP4.2), after the talkers
TX_FIELDS = ("wake why mic_calls mic_pad mic_rate mic_ch mic_open mic_muted gain_q15 talking "
             "spurts queued dropped depth_max peak clock close_ms close_ms_max game_silenced "
             "sent discarded wakes").split()
assert len(TX_FIELDS) == 22  # TX_WORDS in team_360.c
NWORDS = NWORDS_V1 + len(TX_FIELDS)
CONTROLS = HEAD[2:16] + ["wake"]
COUNTERS = HEAD[HEAD.index("err_xnet"):HEAD.index("tone_skips") + 1]
TX_COUNTERS = ("mic_calls spurts queued dropped depth_max peak close_ms close_ms_max "
               "game_silenced sent discarded wakes").split()
TX_WHY = {0: "sending", 1: "stage < 4", 2: "no channel", 3: "MIC TO GAME", 4: "test tone 1",
          5: "socket not open", 6: "quiesced", 7: "another pad", 8: "mic format"}
NET_STATES = {0: "closed", 1: "open", 2: "failed (retrying every 5 s)", 3: "refused (exports)"}
C_STATES = {0: "idle", 1: "buffering", 2: "playing"}
STEPS = {0: "-", 1: "socket", 2: "setsockopt 0x5801", 3: "SO_BROADCAST", 4: "SO_REUSEADDR",
         5: "SO_RCVBUF", 6: "FIONBIO", 7: "bind", 0x80: "startup"}
INIT_ERRORS = {0: "ok", 1: "no xam.xex", 2: "an ordinal didn't resolve", 3: "not the 17559 code"}


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def ip_str(v):
    return ".".join(str((v >> s) & 255) for s in (24, 16, 8, 0))


def ip_int(s):
    if s in ("0", "", "broadcast"):
        return 0
    if s.startswith("0x"):
        return int(s, 16)
    return struct.unpack(">I", socket.inet_aton(s))[0]


# ---- console side --------------------------------------------------------------------------
def block_addr():
    from xa import xw_block
    return xw_block(XT_MAGIC)


def read_block(x, addr):
    w = x.u32s(addr, len(HEAD))
    if not w or w[0] != XT_MAGIC:
        raise SystemExit(f"XWTV block not found at {addr:08x} (xw.xex with team_360.c loaded?)")
    if w[1] not in (1, 2):
        raise SystemExit(f"XWTV version {w[1]}: this team.py knows versions 1 and 2")
    w = x.u32s(addr, NWORDS if w[1] >= 2 else NWORDS_V1)
    g = dict(zip(HEAD, w))
    o = len(HEAD)
    g["peers"] = []
    for i in range(PEERS):
        p = dict(zip(PEER_FIELDS, w[o:o + len(PEER_FIELDS)]))
        o += len(PEER_FIELDS)
        if p["id"]:
            p["name"] = struct.pack(">4I", p["name0"], p["name1"], p["name2"], p["name3"]) \
                .split(b"\0")[0].decode(errors="replace")
            p["slot"] = i
            g["peers"].append(p)
    g["talkers"] = []
    for t in range(TALKERS):
        g["talkers"].append(dict(zip(TALKER_FIELDS, w[o:o + len(TALKER_FIELDS)])))
        o += len(TALKER_FIELDS)
    g["tx"] = dict(zip(TX_FIELDS, w[o:o + len(TX_FIELDS)])) if g["version"] >= 2 else None
    return g


def connect():
    from xa import Xbdm
    addr = block_addr()
    x = Xbdm()
    return x, addr


def read(x=None, addr=None):
    if x is None:
        x, addr = connect()
    return read_block(x, addr)


def field_offset(name):
    """byte offset of a head field, or of a version 2 send-path field (TX_FIELDS)"""
    if name in HEAD:
        return 4 * HEAD.index(name)
    if name in TX_FIELDS:
        return 4 * (NWORDS_V1 + TX_FIELDS.index(name))
    raise SystemExit(f"unknown field {name}")


def setctl(x, addr, name, value):
    return x.setu32(addr + field_offset(name), value & 0xFFFFFFFF)


def print_status(g):
    print(f"stage {g['stage']} (applied {g['applied_stage']}), channel {g['channel'] or 'off'}, "
          f"level {g['level']}, mic to {['TEAM', 'GAME', 'BOTH'][g['mic_route']] if g['mic_route'] < 3 else g['mic_route']}, "
          f"test tone {g['test_tone']}, dest {ip_str(g['dest']) if g['dest'] else '255.255.255.255'}"
          f":{g['port'] or XT_PORT}, opts {g['opts']:x}, startup {g['startup']:x}, loop_own "
          f"{g['loop_own']}, jb_ms {g['jb_ms'] or 'adaptive'}")
    print(f"exports: {INIT_ERRORS.get(g['init_error'], g['init_error'])} ({g['init_detail']:08x}); "
          f"thread {'running' if g['thread'] else 'not started'}, heartbeat {g['heartbeat']}, "
          f"hw thread {g['hw_thread']} priority {g['priority']}; quiesce {g['quiesce']}")
    print(f"socket: {NET_STATES.get(g['net_state'], g['net_state'])} (handle {g['socket']:08x}), "
          f"opened {g['opens']} times, reopened {g['reopens']}; XNetStartup {g['err_xnet']} "
          f"WSAStartup {g['err_wsa']}; open error {g['err_open']} at "
          f"{STEPS.get(g['open_step'], g['open_step'])}")
    print(f"this console: {ip_str(g['ip'])}, id {g['sender']:08x} (MAC {g['mac_hi']:04x}...), "
          f"xnaddr flags {g['xnaddr_flags']:x}, link {g['link']:x}; title switches "
          f"{g['title_switches']}")
    avg = g["tx_us_sum"] / g["tx_calls"] if g["tx_calls"] else 0
    print(f"sent: {g['tx_hello']} hello, {g['tx_audio']} audio, {g['tx_bye']} bye, "
          f"{g['tx_bytes']} bytes; sendto {avg:.0f} us average, {g['tx_us_max']} max, last "
          f"{g['tx_us_last']}; failures {g['send_fails']} (last error {g['err_send']}); tone "
          f"skips {g['tone_skips']}")
    print(f"received: {g['rx_raw']} datagrams ({g['rx_bytes']} bytes): {g['rx_hello']} hello, "
          f"{g['rx_audio']} audio, {g['rx_bye']} bye, {g['rx_bad']} bad, {g['rx_own']} own, "
          f"{g['rx_other_ch']} other channel, {g['rx_no_talker']} no talker slot; failures "
          f"{g['recv_fails']} (last error {g['err_recv']}); recvfrom max {g['rx_us_max']} us")
    print(f"peers: {g['peers_known']} known, {g['team_peers']} consoles on our channel, talking "
          f"mask {g['talking_mask']:x}")
    for p in g["peers"]:
        ch = p["chan"] & 255
        fl = (p["chan"] >> 8) & 255
        print(f"  {p['id']:08x} {ip_str(p['ip']):15s} {p['name']:16s} ch {ch} flags {fl:x} age "
              f"{p['age_ms']} ms; hello {p['rx_hello']} audio {p['rx_audio']} lost {p['lost']} "
              f"late {p['late']} jitter {p['jitter_us'] / 1000:.1f} ms talker "
              f"{p['talker'] - 1 if p['talker'] else '-'}")
    print(f"mix: {g['mix_calls']} blocks for pad {s32(g['mix_pad'])} at {g['rate_out']} Hz x "
          f"{g['ch_out']}, gain {g['gain']}/256, peak {g['out_peak']}; dump {g['dump']}")
    tx = g.get("tx")
    if tx:
        print(f"send (mic): {TX_WHY.get(tx['why'], tx['why'])}; pad {s32(tx['mic_pad'])} at "
              f"{tx['mic_rate']} Hz x {tx['mic_ch']}, {tx['mic_calls']} mic packets; gate "
              f"{'open' if tx['mic_open'] else 'closed'}, {'muted' if tx['mic_muted'] else 'not muted'}, "
              f"level {tx['gain_q15'] * 100 // 32768} %; {'TALKING' if tx['talking'] else 'quiet'}")
        print(f"  {tx['spurts']} talk spurts, {tx['queued']} packets queued, {tx['sent']} sent, "
              f"{tx['discarded']} discarded, {tx['dropped']} dropped (ring full; deepest "
              f"{tx['depth_max']}); peak {tx['peak']}; gate close to last packet {tx['close_ms']} "
              f"ms (max {tx['close_ms_max']}); net wakes {tx['wakes']} (wake {tx['wake']}); "
              f"360 given silence {tx['game_silenced']} times (MIC TO TEAM)")
    for i, t in enumerate(g["talkers"]):
        if not t["id"] and not t["starts"]:
            continue
        print(f"  talker {i}: {t['id']:08x} {C_STATES.get(t['state'], t['state'])}, fill "
              f"{t['fill_ms']} ms (target {t['target_ms']}), jitter {t['jitter_us'] / 1000:.1f} ms, "
              f"trim {s32(t['trim_ppm'])} ppm; underruns {t['underruns']} overruns "
              f"{t['overruns']} starts {t['starts']} spurts {t['spurts']} restarts "
              f"{t['restarts']}; concealed {t['concealed']} recovered {t['recovered']} late "
              f"{t['late']}; mixed {t['mixed_ms'] / 1000:.1f} s, peak {t['peak']}")


def goertzel(samples, rate, hz):
    """relative amplitude of hz in samples (0..1 for a full-scale sine)"""
    n = len(samples)
    if not n:
        return 0.0
    k = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for v in samples:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - k * s1 * s2
    return 2 * math.sqrt(max(p, 0)) / n / 32768


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def console_dump(path, hz=440):
    """arms the team mix dump, waits for it, writes a WAV; returns (samples, rate, amp, rms)"""
    x, addr = connect()
    setctl(x, addr, "dump", 1)
    end = time.time() + 5
    g = read_block(x, addr)
    while time.time() < end and g["dump"] != 3:
        time.sleep(0.25)
        g = read_block(x, addr)
    if g["dump"] != 3:
        raise SystemExit(f"dump didn't finish (state {g['dump']}: stage 3, a channel, a talker "
                         f"and a streaming headset are needed)")
    n, at, rate = g["dump_frames"], g["dump_addr"], g["dump_rate"]
    data = b""
    while len(data) < 2 * n:
        data += x.mem(at + len(data), min(0x1000, 2 * n - len(data)))
    setctl(x, addr, "dump", 0)
    s = list(struct.unpack(f">{n}h", data[:2 * n]))
    if path:
        write_wav(path, s, rate)
    amp = goertzel(s, rate, hz)
    rms = math.sqrt(sum(v * v for v in s) / len(s)) / 32768 if s else 0
    return s, rate, amp, rms


# ---- the PC peer --------------------------------------------------------------------------
def pack(kind, channel, flags, sender, seq, samples=0, ts=0, payload=b""):
    return HDR.pack(XT_MAGIC, XT_VERSION, kind, channel, flags, sender, seq & 0xFFFF, samples,
                    ts & 0xFFFFFFFF) + payload


def unpack(data):
    """(header dict, payload) or None if it isn't ours"""
    if len(data) < HDR.size:
        return None
    m, v, kind, ch, fl, sender, seq, n, ts = HDR.unpack_from(data)
    if m != XT_MAGIC or v != XT_VERSION or kind not in TYPES:
        return None
    return dict(type=kind, channel=ch, flags=fl, sender=sender, seq=seq, samples=n,
                timestamp=ts), data[HDR.size:]


class Peer:
    """a team voice peer on this machine: one UDP socket on the team port"""

    def __init__(self, dest="255.255.255.255", port=XT_PORT, sender=None, name="MAC",
                 flags=F_PC):
        self.dest, self.port = dest, port
        self.sender = sender if sender is not None else (random.getrandbits(31) | 0x40000000)
        self.name, self.flags = name, flags
        self.seq = {HELLO: 0, AUDIO: 0, BYE: 0}
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.s.bind(("", port))
        self.s.setblocking(False)

    def send(self, kind, channel, samples=0, ts=0, payload=b"", flags=0, sender=None):
        self.s.sendto(pack(kind, channel, self.flags | flags, self.sender if sender is None else
                           sender, self.seq[kind], samples, ts, payload), (self.dest, self.port))
        self.seq[kind] += 1

    def hello(self, channel, level=0):
        self.send(HELLO, channel, payload=HELLO_PAYLOAD.pack(self.name.encode()[:15], level))

    def bye(self, channel):
        self.send(BYE, channel)

    def audio(self, channel, pcm, ts, flags=F_TALKING):
        self.send(AUDIO, channel, len(pcm), ts, struct.pack(f">{len(pcm)}h", *pcm), flags)

    def recv_all(self):
        """[(header, payload, (ip, port), arrival time)] of everything queued, our own dropped"""
        out = []
        while True:
            try:
                data, src = self.s.recvfrom(2048)
            except (BlockingIOError, InterruptedError):
                return out
            u = unpack(data)
            if u and u[0]["sender"] != self.sender:
                out.append((u[0], u[1], src, time.monotonic()))

    def close(self):
        self.s.close()


class Tone:
    """a paced 16 kHz sine: next(n) gives the samples due"""

    def __init__(self, hz=440.0, dbfs=-20.0):
        self.inc = 2 * math.pi * hz / RATE
        self.amp = 32767 * 10 ** (dbfs / 20)
        self.ph = 0.0

    def packet(self):
        out = []
        for _ in range(PACKET):
            out.append(int(round(self.amp * math.sin(self.ph))))
            self.ph += self.inc
        return out


class Summary:
    """per-sender AUDIO statistics, as the console's jitter buffer would see them"""

    def __init__(self):
        self.s = {}

    def add(self, h, t):
        d = self.s.setdefault(h["sender"], dict(n=0, lost=0, dup=0, reorder=0, last_seq=None,
                                                first=t, last=t, max_gap=0.0, spurts=1,
                                                channel=h["channel"]))
        if d["last_seq"] is not None:
            diff = (h["seq"] - d["last_seq"]) & 0xFFFF
            if diff == 0:
                d["dup"] += 1
            elif diff < 0x8000:
                d["lost"] += diff - 1
            else:
                d["reorder"] += 1
                d["lost"] = max(0, d["lost"] - 1)
            gap = t - d["last"]
            d["max_gap"] = max(d["max_gap"], gap)
            if gap > 0.25:
                d["spurts"] += 1
        if d["last_seq"] is None or ((h["seq"] - d["last_seq"]) & 0xFFFF) < 0x8000:
            d["last_seq"] = h["seq"]
        d["n"] += 1
        d["last"] = t
        d["channel"] = h["channel"]

    def lines(self):
        for sid, d in self.s.items():
            span = max(d["last"] - d["first"], 1e-6)
            yield (f"  {sid:08x} ch {d['channel']}: {d['n']} packets ({d['n'] / span:.1f}/s), lost "
                   f"{d['lost']}, duplicate {d['dup']}, reordered {d['reorder']}, longest gap "
                   f"{d['max_gap'] * 1000:.0f} ms, talk spurts {d['spurts']}")


class Background:
    """a PC peer running in a thread (for the acceptance scripts): HELLO once a second on
    channel, and with tone=True a paced 440 Hz AUDIO stream; records what it hears.
    hellos: {sender: [arrival times]}; audio: Summary; byes: [sender]"""

    def __init__(self, channel=1, tone=False, dest="255.255.255.255", port=XT_PORT,
                 name="ACCEPT", hz=440.0, dbfs=-20.0, keep_audio=False):
        import threading
        self.peer = Peer(dest, port, name=name)
        self.channel, self.tone = channel, Tone(hz, dbfs) if tone else None
        self.hellos, self.byes, self.audio = {}, [], Summary()
        self.hello_flags = {}  # sender: the flags of its last HELLO
        self.packets = [] if keep_audio else None  # [(header, payload, arrival)] of AUDIO
        self.sent_hello = self.sent_audio = 0
        self.skips = 0
        self.stop_flag = False
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.t.start()
        return self

    def run(self):
        start = time.monotonic()
        next_hello = start
        next_pkt = start
        ts = random.getrandbits(32)
        while not self.stop_flag:
            now = time.monotonic()
            if now >= next_hello:
                self.peer.hello(self.channel)
                self.sent_hello += 1
                next_hello += 1.0
            if self.tone:
                if now - next_pkt > 0.2:
                    skip = int((now - next_pkt) * RATE / PACKET)
                    next_pkt += skip * PACKET / RATE
                    ts += skip * PACKET
                    self.skips += 1
                while now >= next_pkt:
                    self.peer.audio(self.channel, self.tone.packet(), ts)
                    self.sent_audio += 1
                    ts += PACKET
                    next_pkt += PACKET / RATE
            for h, payload, src, t in self.peer.recv_all():
                with self.lock:
                    if h["type"] == HELLO:
                        self.hellos.setdefault(h["sender"], []).append(t)
                        self.hello_flags[h["sender"]] = h["flags"]
                    elif h["type"] == BYE:
                        self.byes.append(h["sender"])
                    else:
                        self.audio.add(h, t)
                        if self.packets is not None:
                            self.packets.append((h, payload, t))
            time.sleep(0.001)
        self.peer.bye(self.channel)
        self.peer.close()

    def stop(self):
        self.stop_flag = True
        self.t.join(timeout=5)


def describe(h, payload, src):
    s = (f"{TYPES[h['type']]:5s} from {src[0]} id {h['sender']:08x} ch {h['channel']} flags "
         f"{h['flags']:x} seq {h['seq']}")
    if h["type"] == HELLO and len(payload) >= HELLO_PAYLOAD.size:
        name, level = HELLO_PAYLOAD.unpack_from(payload)
        name = name.split(b"\0")[0].decode(errors="replace")
        s += f" name {name!r} level {level}"
    return s


def run_peer(args, mode):
    p = Peer(args.dest, args.port, int(args.id, 16) if args.id else None, args.name)
    ch = args.channel
    secs = args.secs
    print(f"peer {p.sender:08x} ({args.name}) on UDP {args.port}, sending to {args.dest}; "
          f"mode {mode}" + (f", channel {ch}" if mode != "listen" else ""), flush=True)
    tone = Tone(args.hz, args.dbfs) if mode == "tone" else None
    rec = {} if mode == "record" else None
    summ = Summary()
    seen = {}
    reflected = set()
    start = time.monotonic()
    next_hello = start
    next_pkt = start
    next_sum = start + 5
    ts = random.getrandbits(32)
    sent = 0
    try:
        while secs is None or time.monotonic() - start < secs:
            now = time.monotonic()
            if mode in ("hello", "tone") and now >= next_hello:
                p.hello(ch)
                next_hello += 1.0
            if tone:
                while now >= next_pkt:
                    p.audio(ch, tone.packet(), ts)
                    ts += PACKET
                    sent += 1
                    next_pkt += PACKET / RATE
                    if now - next_pkt > 0.2:  # this process was held up: skip ahead
                        skip = int((now - next_pkt) * RATE / PACKET)
                        next_pkt += skip * PACKET / RATE
                        ts += skip * PACKET
            for h, payload, src, t in p.recv_all():
                if h["sender"] in reflected:
                    continue  # our own reflection, looped back: never reflect it again
                if mode == "reflect":
                    reflected.add(h["sender"] ^ 0x80000000)
                if h["type"] == AUDIO:
                    summ.add(h, t)
                    if mode == "reflect":
                        p.send(AUDIO, h["channel"], h["samples"], h["timestamp"], payload,
                               sender=h["sender"] ^ 0x80000000)
                    if rec is not None and (h["channel"] == ch or h["flags"] & F_ALLCALL):
                        rec.setdefault(h["sender"], []).append((h["timestamp"], payload, t))
                else:
                    key = (h["sender"], h["type"], h["channel"])
                    if mode == "reflect" and h["type"] == HELLO:
                        p.send(HELLO, h["channel"], payload=payload,
                               sender=h["sender"] ^ 0x80000000)
                    n = seen.get(key, 0)
                    seen[key] = n + 1
                    if n == 0 or h["type"] == BYE or args.verbose:
                        print(f"{now - start:7.2f} " + describe(h, payload, src), flush=True)
            if now >= next_sum:
                next_sum += 5
                hello_line = ", ".join(f"{k[0]:08x} ch {k[2]}: {v}" for k, v in seen.items()
                                       if k[1] == HELLO)
                print(f"{now - start:7.2f} hellos heard: {hello_line or 'none'}"
                      + (f"; sent {sent} audio packets" if tone else ""), flush=True)
                for line in summ.lines():
                    print(line, flush=True)
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    if mode in ("hello", "tone"):
        p.bye(ch)
    print("summary:", flush=True)
    for k, v in seen.items():
        print(f"  {TYPES[k[1]]} from {k[0]:08x} ch {k[2]}: {v}")
    for line in summ.lines():
        print(line)
    if rec is not None:
        save_record(rec, args.file)
    p.close()
    return seen, summ


def save_record(rec, path):
    """every sender's packets placed by its timestamps, aligned by arrival, mixed, 16 kHz"""
    if not rec:
        print("record: nothing received on that channel")
        write_wav(path, [], RATE)
        return
    t0 = min(pk[0][2] for pk in rec.values())
    tracks = []
    for sid, pkts in rec.items():
        base_ts, _, base_t = pkts[0]
        off = int((base_t - t0) * RATE)
        tr = {}
        for ts, payload, _ in pkts:
            n = len(payload) // 2
            pos = off + ((ts - base_ts) & 0xFFFFFFFF if ((ts - base_ts) & 0xFFFFFFFF) < 1 << 31
                         else -((base_ts - ts) & 0xFFFFFFFF))
            tr[pos] = struct.unpack(f">{n}h", payload[:2 * n])
        tracks.append(tr)
    end = max(pos + len(v) for tr in tracks for pos, v in tr.items())
    mix = [0] * max(end, 0)
    for tr in tracks:
        for pos, v in tr.items():
            for i, s in enumerate(v):
                if 0 <= pos + i < len(mix):
                    mix[pos + i] += s
    mix = [max(-32768, min(32767, s)) for s in mix]
    write_wav(path, mix, RATE)
    print(f"record: {len(mix) / RATE:.2f} s from {len(rec)} sender(s) -> {path}; 440 Hz "
          f"amplitude {goertzel(mix, RATE, 440):.4f}")


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("cmd", nargs="?", default="status")
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--dest", default="255.255.255.255")
    ap.add_argument("--port", type=int, default=XT_PORT)
    ap.add_argument("--hz", type=float, default=440.0)
    ap.add_argument("--dbfs", type=float, default=-20.0)
    ap.add_argument("--name", default="MAC-PEER")
    ap.add_argument("--id", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("-h", "--help", action="store_true")
    a = ap.parse_args()
    if a.help:
        print(__doc__)
        return
    r = a.rest
    c = a.cmd
    if c in ("listen", "hello", "tone", "record", "reflect"):
        a.channel = 1
        a.secs = None
        a.file = None
        if c == "listen" or c == "reflect":
            a.secs = float(r[0]) if r else None
        elif c == "hello":
            a.channel = int(r[0]) if r else 1
            a.secs = float(r[1]) if len(r) > 1 else None
        elif c == "tone":
            if not r:
                raise SystemExit("tone CH [SECS]")
            a.channel = int(r[0])
            a.secs = float(r[1]) if len(r) > 1 else None
        else:
            if len(r) < 2:
                raise SystemExit("record CH FILE.wav [SECS]")
            a.channel, a.file = int(r[0]), r[1]
            a.secs = float(r[2]) if len(r) > 2 else 10.0
        run_peer(a, c)
        return
    if c == "status":
        print_status(read())
    elif c == "stage":
        x, addr = connect()
        if r[1:]:
            print("channel =", int(r[1], 0), setctl(x, addr, "channel", int(r[1], 0)))
        print("stage =", int(r[0], 0), setctl(x, addr, "stage", int(r[0], 0)))
    elif c == "set":
        x, addr = connect()
        name, v = r[0], r[1]
        if name not in CONTROLS and name != "dump":
            raise SystemExit(f"not a control: {name}; one of {' '.join(CONTROLS)}")
        val = ip_int(v) if name == "dest" else int(v, 0)
        print(name, "=", v, setctl(x, addr, name, val))
    elif c == "zero":
        x, addr = connect()
        g = read_block(x, addr)
        for name in COUNTERS + (TX_COUNTERS if g["tx"] else []):
            setctl(x, addr, name, 0)
        print("counters zeroed")
    elif c == "dump":
        if not r:
            raise SystemExit("dump FILE.wav")
        s, rate, amp, rms = console_dump(r[0], a.hz)
        print(f"{len(s)} samples at {rate} Hz -> {r[0]}; {a.hz:.0f} Hz amplitude {amp:.4f} "
              f"({20 * math.log10(amp) if amp > 0 else -999:.1f} dBFS), rms {rms:.4f}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
