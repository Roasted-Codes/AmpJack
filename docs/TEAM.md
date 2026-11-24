# Team voice (console to console)

The MixAmp daisy chain without the cables: consoles on the same LAN pick a channel (1-8) and
hear each other in the headset, mixed under game audio. No PC, no server, no pairing. Code:
`xbox360/team_360.c` (the console side), `xbox360/net_360.c` (sockets), `xbox360/team.py`
(status, and a PC peer that speaks the protocol). Plan: `docs/PLAN.md` Phase 4.

Everything is off at boot: `XWTV.stage` is 0, so no thread and no socket exist until it is
raised over xbdm (`team.py stage N`).

## The protocol (version 1)

So that a PC, phone or OBS client can join, the whole protocol is this section.

- **Transport:** UDP, port **3660**, IPv4 broadcast to 255.255.255.255 (a console can be set to
  a subnet broadcast or one unicast address instead: `team.py set dest A.B.C.D`). Send from
  port 3660 and listen on 3660 (`SO_REUSEADDR`, `SO_BROADCAST`).
- **Byte order:** big-endian for every field and for the PCM.
- **No encryption, no authentication.** Anyone on the LAN can listen and talk. Fine for a
  private LAN; don't run it on an untrusted one.

Every packet starts with a 20-byte header:

| Offset | Size | Field | |
|---|---|---|---|
| 0 | 4 | magic | `0x58575456` ("XWTV") |
| 4 | 1 | version | 1; a receiver drops other versions |
| 5 | 1 | type | 1 HELLO, 2 AUDIO, 3 BYE |
| 6 | 1 | channel | 1..8; 0 = none (a console with no channel picked) |
| 7 | 1 | flags | 1 TALKING, 2 MUTED, 4 PC (not a console), 8 ALLCALL (heard on every channel) |
| 8 | 4 | sender | stable per device: a console uses the low 4 bytes of its MAC address |
| 12 | 2 | seq | per sender and type, +1 per packet, wraps |
| 14 | 2 | samples | AUDIO: sample frames that follow; otherwise 0 |
| 16 | 4 | timestamp | AUDIO: the sender's sample clock at 16 kHz (+samples per packet); otherwise 0 |

- **HELLO** (40 bytes): header + `char name[16]` (NUL-padded; consoles send `XBOX-xxxx` for
  now), `u8 level` (0, reserved), 3 bytes 0. Sent once a second while the device is on, and
  at once when its channel changes. A peer not heard for 5 s is dropped.
- **AUDIO** (20 + 2 x samples bytes): 16 kHz mono s16 big-endian PCM. Consoles send 160
  samples (10 ms, 340 bytes); receivers accept 1..480 samples. Sent only while the talker is
  talking (the mic gate open, plus a 200 ms hang), so silence costs nothing. A receiver places
  a packet by its timestamp; a jump of more than 200 ms, or no packet for 80 ms, starts a new
  talk spurt.
- **BYE** (20 bytes): header only, when the device leaves. Drop the sender at once.

Rules a receiver follows:
- drop packets whose `sender` is its own (broadcast loops back on many stacks);
- play AUDIO only if its channel equals the receiver's channel, or it has ALLCALL;
- never send received audio back out (no network echo path exists by construction).

A PC listener (OBS, admin) needs no setup: every channel is on the wire. Admin talk-back is
AUDIO with flags PC | ALLCALL (or PC and one channel).

## On the console

The `XWTV` block (`team.py status`; all u32, the field list is `team.py`'s `HEAD`; version 2
appends the send path's `TX_FIELDS` after the talkers):

| Control | Default | |
|---|---|---|
| `stage` | 0 | 0 off, 1 socket only (datagrams counted, not parsed), 2 HELLO and discovery, 3 receive (mix into the earpiece), 4 send |
| `channel` | 0 | TEAM CH: 0 off, 1..8 |
| `level` | 10 | TEAM VOL 0..10 |
| `mic_route` | 0 | MIC TO: 0 TEAM (the team gets the mic, the 360 gets silence), 1 GAME (the team gets nothing), 2 BOTH |
| `test_tone` | 0 | stage 4, instead of the mic: 1 = a 440 Hz stream (-20 dBFS) from the net thread; 2 = a synthetic mic (440 Hz, -20 dBFS, gated 1 s on / 1 s off by the mic's clock, mute obeyed) through the whole send path |
| `dest` | 0 | 0 = 255.255.255.255, else this IPv4 address (subnet broadcast or unicast) |
| `port` | 0 | 0 = 3660 |
| `opts` | 7 | 1 insecure (option 0x5801), 2 SO_BROADCAST, 4 SO_REUSEADDR; a change reopens the socket |
| `startup` | 3 | 1 XNetStartup, 2 WSAStartup (system caller); a change reopens |
| `loop_own` | 0 | 1: if the stack loops our own broadcasts back, play them (a one-console test) |
| `jb_ms` | 0 | jitter buffer target: 0 adaptive (30..80 ms), else fixed |
| `hw_thread`, `priority` | 5, 8 | the net thread (read when it is created; priority any time) |
| `debug` | 0 | 1: log every new peer and talker |
| `wake` (tx) | 1 | 1: the net thread waits on an event each queued mic packet sets (0: it only polls) |

The earpiece mix: `earpiece = master x (game x game level + (chat + team x team level) x chat
level) + sidetone`; voice_360.c passes `xg_chat_gain() x level / 10` to `xt_mix`. Up to 16
peers are tracked (any channel) and 4 talkers are mixed at once.

Receive path per talker: a 256 ms ring of 16 kHz samples, a target fill of 15 ms + 3 x the
measured interarrival jitter (30..80 ms), a lost packet concealed by repeating the last one
at falling gain, a late one used if it is still ahead of the reader, and a linear resampler
to the earpiece rate whose ratio is trimmed (at most 1000 ppm, as `game_360.c`) to keep the
fill at the target, because the sender's clock is not ours. Simulated on the Mac by
`tests/test_team.c` (`cc -O2 -Wall -o test_team tests/test_team.c -lm && ./test_team`).

Send path (WP4.2): voice_360.c calls `xt_mic` with every 1 ms mic packet and the noise gate's
decision (`xd_gate_open`: closed while muted). On the driver thread, team_360.c mixes it to
mono, applies the mic level in effect (`XWDG.applied_mic_gain`) and the gate, both ramped over
10 ms as voice_360.c does for the 360, resamples to 16 kHz (the same integer resampler as
voice_360.c) and cuts 160-sample packets on a running 16 kHz sample clock (the timestamp; a
pause shows as a jump). A packet goes out if any of its samples had a gain above 0, so a talk
spurt ends 10 ms after the gate closes; the gate's own 200 ms hold is the hang. Packets go to
the net thread through a 32-packet lock-free ring (320 ms; a full ring counts `dropped`), and
an event wakes the net thread at once instead of at its next poll. HELLO carries TALKING while
the mic is sent and MUTED while it is muted. `xt_mic_to_game` is false (the 360 gets the mute
path's silence) only on MIC TO TEAM at stage 4 with a channel and the socket open, so a failed
socket never takes the mic from the game. One headset per console: the first pad whose mic
arrives is sent; another takes over after 200 ms without it. `tx` state: `why` (0 sending, 1
stage < 4, 2 no channel, 3 MIC TO GAME, 4 test tone 1, 5 socket not open, 6 quiesced, 7 another
pad, 8 mic format), the mic's pad / rate / gate / mute / level, `spurts`, `queued`, `sent`,
`discarded`, `dropped`, `depth_max`, `peak`, `close_ms(_max)` (the gate closing to the end of
the last packet), `game_silenced` (MIC TO TEAM's silences), `wakes`. Simulated by
`tests/test_team_tx.c` (`cc -O2 -Wall -o test_team_tx tests/test_team_tx.c -lm && ./test_team_tx`).

## What WP4.0 read offline and from the console (read-only)

- The NetDll ordinals in the 17559 XAM's export table (`research/dumps/xam_815f0000.bin`,
  table at 0x81A73D34): WSAStartup 1, WSACleanup 2, socket 3, closesocket 4, ioctlsocket 6,
  setsockopt 7, bind 11, select 15, recvfrom 20, sendto 24, WSAGetLastError 27, XNetStartup 51,
  XNetCleanup 52, XNetGetTitleXnAddr 73, XNetGetEthernetLinkStatus 75: all as Xenia's table,
  with the argument counts the export stubs shift. `net_360.c` checks each address and its
  first four words before use.
- XAM keeps separate XNet and WSA start counts per caller (title 1, system 2; other callers
  are refused). `socket()` needs the caller's WSA count, not its XNet count.
- Option 0x5801 checks the insecure-sockets privilege only for a title caller; a system
  caller is never refused.
- HaloLAN.xex (in memory at 0x91A20000) uses caller 2 for every NetDll call: XNetStartup with
  the bypass-security flag, WSAStartupEx (ord 36), UDP sockets with 0x5801, SO_RCVBUF 128 KB,
  bound to random ports from 20000 (and a TCP socket). xbdm.xex uses caller 2 too, with
  0x5801, SO_REUSEADDR and SO_BROADCAST, and starts nothing itself. JRPC2.xex has no NetDll
  import. (HaloLAN's ports are 20000 and up, xbdm's 730 / 731; whether anything else binds
  3660 shows as a bind error at stage 1.)

Still to be seen on the console (`accept/p4_net.py --spike` answers them): whether caller 2
needs our own startup at all; whether a socket without 0x5801 still reaches the Mac; whether
255.255.255.255 and the subnet broadcast both go out and come in; whether the stack loops our
own broadcasts back; the cost of a sendto; that the socket survives title switches and a
Halo 3 system-link lobby.

## Test steps (coordinator)

One console, so the Mac is the second peer (`team.py` on the Mac at the LAN address, UDP 3660;
allow it through the Mac firewall if asked).

1. Install this build of xw.xex, reboot, press the pad's Xbox button (as for WP0.1).
2. `XBOX=10.0.0.13 python3 xbox360/accept/p4_net.py --spike --switches 2`: switch Aurora ->
   Halo 3 -> Aurora when it says so. Then the OWNER CHECK: a Halo 3 system-link lobby works
   with stage 2 on.
3. Headset streaming (`xw.py audio 4`), then
   `XBOX=10.0.0.13 python3 xbox360/accept/p4_rx.py` (5 minutes; `--hands` if the owner listens).
4. Send path: `XBOX=10.0.0.13 python3 xbox360/accept/p4_tx.py` (headset streaming; about a
   minute, no hands: the synthetic mic, MIC TO, stage 3, an injected mute), then with the owner at
   the headset and a Halo 3 lobby open (so the 360 takes the mic) `... p4_tx.py --hands`: he
   talks, stays silent and talks muted when the DO lines say so, then hears his own recorded
   words played back (OWNER CHECK: clean).
5. Optional one-console round trip: `team.py stage 4 1`, `team.py set test_tone 1`, then
   `team.py record 1 out.wav 10` on the Mac records the console's tone; `team.py reflect` on
   the Mac plays it back to the console as a second talker (stage 3 or 4 hears it).
