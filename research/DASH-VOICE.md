# The system's Voice settings and our headset panel

Research notes, Nov 2026. The Guide's voice page (Settings > Preferences > Voice, also in the
Guide during a game) has three controls: **Voice Volume** (1..10), **Game Volume** (1..10) and
**Voice Output** (Headset / Speakers / Both). Our panel has BALANCE, VOLUME and mic mute. This
note says where each system setting lives, where it acts on the sound, how that meets our
plugin, and what the plugin should do so a player has one coherent set of controls.

Sources: the 17559 kernel and XAM dumps (`research/dumps/`), read with capstone; the Guide
(`hud.xex`) and dashboard (`dash.xex`) images copied read-only from the running console with
xbdm `getmem`; live reads of kernel and XAM memory (`research/tools/dashvoice.py`, read only);
Xenia's export tables (`xboxkrnl_table.inc`, `xam_table.inc`) and `user_profile.cc`.

Marks: **VERIFIED** = read in the dump or on the console; **PUBLIC** = from a public source
only (Xenia, SDK names); **GUESS** = reasoning, not seen. The owner's settings (Voice Volume 10,
Game Volume 6, Headset) are the ground truth for the live reads.

## Short answers

| | answer | mark |
|---|---|---|
| Voice Volume | profile setting `0x1004000E`, 0..100, per signed-in profile; multiplies the other players' voices **before** they reach our hook | VERIFIED for XAM's party chat; GUESS for a game's own chat |
| Game Volume | console-wide XConfig byte (category 7, setting 0x0A), 1..10; it is the **level of the kernel ducker**: the whole game mix goes down by 3 dB per step below 10, only while someone talks, and **only when chat plays through the TV speakers** | VERIFIED |
| Voice Output | profile setting `0x1004000D` (0 Headset, 1 Speakers, 2 Both); Speakers stops chat going to the headset device (our chat path) and puts it in the TV mix (our game tap); Both sends it both ways | VERIFIED for party chat; GUESS for a game's own chat |
| Live change | yes: the Guide page writes each change at once; Game Volume applies at once; party chat re-reads the profile settings | VERIFIED (Guide, Game Volume); party chat re-read partly traced; games GUESS |
| With the owner's settings (10 / 6 / Headset) | none of the three changes what our headset plays today | VERIFIED for Game Volume (ducker idle, level -12 dB unused); Voice Volume 10 = gain 1.0 |

## 1. Where each setting lives

### 1.1 Voice Volume and Voice Output: per-profile settings

- Ids (XDK names, PUBLIC from Xenia's `user_profile.cc`): `XPROFILE_OPTION_VOICE_MUTED`
  `0x1004000C`, `XPROFILE_OPTION_VOICE_THRU_SPEAKERS` `0x1004000D`,
  `XPROFILE_OPTION_VOICE_VOLUME` `0x1004000E`. VERIFIED that XAM uses exactly these three, in
  this order, as one array at `0x81A97E74` read by its own voice engine (below).
- Stored in each profile, read under the **dashboard's title id `0xFFFE07D1`** (VERIFIED: XAM's
  engine at `0x81A41CDC` and the Guide page at `0x913F2AF8` both pass it).
- Values (VERIFIED, XAM's engine parse at `0x81A41B10..0x81A41BEC`):
  - Voice Volume: clamped to 100; the gain applied is value x 0.01 (`0x81A41CB4`). Default 100:
    XAM's default-settings table at `0x8162FA20` holds `{0x1004000E, 0x64}` (VERIFIED; Xenia
    has the same, PUBLIC).
  - The Guide slider writes `value = 100 x position / (steps - 1)` (`hud.xex` `0x913F2790`,
    constant 100.0 at `0x913E28A4`; VERIFIED). So step 10 = 100 = gain 1.0 (the owner), and the
    bottom step writes 0 (GUESS that the slider has 10 positions, 1..10).
  - Voice Output: 0 = headset only, 1 = speakers only, 2 = both. In XAM's engine flag word
    (+0xC8): bit 0x2 = "play through speakers" (set for 1 and 2), bit 0x4 = "play to the headset"
    (set for 0 and 2). Default 0 (Xenia, PUBLIC; XAM's engine default flags `0x26404` = volume
    100, headset, VERIFIED).
  - Voice Muted (`0x1004000C`) sets bit 0x1; it is not on this voice page (the page's fourth
    control is a Kinect mute checkbox, `chkMuteKinect`, VERIFIED string in `hud.xex`).
- Nobody signed in: there is no profile to read. XAM's engine then keeps its defaults
  (volume 100, headset), VERIFIED by the failed-read path `0x81A41C0C..0x81A41C30`. Our panel's
  "nobody" record is unaffected.
- Exports (ordinals from XAM's own export table in the dump at `0x81A73D34`, VERIFIED; names
  from Xenia's table, PUBLIC):
  - 537 `XamUserReadProfileSettings` = `0x816B1F28`
  - 538 `XamUserWriteProfileSettings` = `0x816B1968`
  - 554 `XamUserReadProfileSettingsEx`
  - 739 `XamShowVoiceSettingsUI` = `0x816C2B60` (opens this page)
  - (also 551 `XamUserGetSigninInfo` = `0x816DDB20`: this settles the "not proven on 17559"
    note in `settings_360.c`; the dump does contain XAM's export table.)
- Call shape (VERIFIED from XAM's and the Guide's calls): `XamUserReadProfileSettings(title_id
  0xFFFE07D1, user_index, 0, NULL, count, ids[], &size, buffer, overlapped)`: **nine
  arguments**, so our code must use `xa_kcall9` (`docs/INTERNALS.md`). The Guide first passes a
  NULL buffer and gets `0x7A` (insufficient buffer) with the size, then calls again
  (`0x913F2B00..0x913F2B4C`). Result: `{u32 count; setting *}`; each setting is 0x28 bytes, id
  at +0x10, int32 value at +0x20 (VERIFIED: both parsers use these offsets).

### 1.2 Game Volume: one console-wide XConfig byte

- XAM's chat manager (an XMsg app; object pointer at `0x81B2A368`) creates itself at boot and
  reads XConfig **category 7, setting 0x0A, 1 byte** (`0x81902800`). If the read fails it uses
  6 and writes it back. It then calls its setter `0x81901FE8`. VERIFIED.
- The setter (VERIFIED, `0x81901FE8..0x819020A0`): clamps to 1..10 (else 6), stores the step at
  manager+0x98, computes `level = 10 ^ ((10 - step) x -3 x 0.05)` = **-3 dB per step below 10**
  (constants 10.0, -3.0, 0.05 at `0x815F42B0`, `0x8162C750`, `0x815FCC70`), saves the byte with
  `ExSetXConfigSetting(7, 0x0A, &byte, 1)` when asked to persist, and calls
  **`XAudioSetDuckerLevel(level)`**.
- Live on the console (VERIFIED, read only): manager+0x98 = **6**; kernel ducker level =
  **0.2512 = -12.0 dB** (= step 6). It is console-wide: not in any profile, the same for every
  player and for nobody signed in.
- XAM messages (VERIFIED in XAM's dispatcher `0x81901690` and in the Guide): `0x0010001A` = get
  the step, `0x0010001B` = set and save it. The Guide sends them with `XMsgInProcessCall`
  (xam 500, `0x816A2F88`) to app `0xFD` (`hud.xex` `0x913F28B4`, `0x913F2C94`). The slider is
  named `sliderDucking` in the Guide (VERIFIED string), and its value is `1 + 9 x position`.
- Side note: XAM's import of `ExGetXConfigSetting` is patched on this console to jump into
  `launch.xex` (DashLaunch) at `0x91F076B8` (VERIFIED, live thunk bytes). Harmless here.

### 1.3 The kernel ducker

Exports (VERIFIED in the kernel's export table; ordinals match Xenia's, PUBLIC):

| ord | address | name | field (driver D = `0x80195F38`) |
|---|---|---|---|
| 845 | 0x8012CA90 | `XAudioEnableDucker(BOOL)` | request on (D+0x1B4) / off (D+0x1B8) |
| 846 | 0x8012CAC0 | `XAudioSetDuckerLevel(float)` | level D+0x1B0 |
| 847 | 0x8012CAD8 | `XAudioIsDuckerEnabled()` | D+0x1A4 |
| 848 | 0x8012CAF0 | `XAudioGetDuckerLevel()` | D+0x1B0 |
| 849/850 | 0x8012CB08/20 | Get/Set `DuckerThreshold` | D+0x1BC |
| 851/852 | 0x8012CB38/50 | Get/Set `DuckerAttackTime` | D+0x1C0 (s) |
| 853/854 | 0x8012CB68/80 | Get/Set `DuckerReleaseTime` | D+0x1C4 (s) |
| 855/856 | 0x8012CB98/B0 | Get/Set `DuckerHoldTime` | D+0x1C8 (s) |

Level, threshold and times are floats (the getters return in f1): we cannot call them (no
floating point), but we can read the fields as raw bits. Live values (VERIFIED): level 0.2512,
threshold 0.00316 (mean square, about -25 dBFS RMS), attack 0.05 s, release 0.25 s, hold 0.5 s,
state 0 (not ducking).

Who calls them (VERIFIED, XAM import thunks): XAM imports Enable, SetLevel, GetLevel,
GetThreshold, GetAttack, GetRelease, GetHold. Nothing else in the kernel or XAM calls them. A
game could import them only if built with a late XDK (GUESS: none seen; no game image dumped).

Not the ducker: `XAudioGet/SetVoiceCategoryVolume` (503-505) are XAudio voice categories (XAM's
music player uses them, GUESS), not voice chat.

## 2. Where they act: the two paths in words

```
OTHER PLAYERS' VOICES
  game chat:  game's own XHV library (in the game) -------------------------+
  party chat: XAM's voice engine (0x81A4xxxx), per local player:            |
                 voice x Voice Volume/100 ............ [Voice Volume]        |
                 Voice Output has "headset"? .......... [Voice Output]       |
                   yes -> float to s16 -> XamVoiceSubmitPacket --------------+
                   speakers (or no headset / Kinect) -> XAM's render frame --|--> (TV path below)
                                                                            v
          XVoicedSubmitPacket  <== OUR HOOK (voice_360.c) takes the render packets here
                                                                            |
          our chat path: resample -> BALANCE chat % -> VOLUME -> headset ear (+ sidetone)

GAME SOUND
  game's XAudio2 frame + XAM's frame (Guide sounds, party chat on speakers)
     -> kernel render thread sums them (XAudioSubmitRenderDriverFrame)
     -> DUCKER (0x8012D158): whole mix x gain ............. [Game Volume = ducker level]
        (gain 1.0 unless XAM turned it on this frame)
     -> process-frame callback (if any)
     -> MEC clients  <== OUR GAME TAP (game_360.c, XAudioCaptureRenderDriverFrame)
     |                    -> downmix -> BALANCE game % -> VOLUME -> headset ear
     -> hardware -> TV
```

- The ducker runs at step 4 of the render loop (`bl 0x8012D158` at `0x8012E538`), before the
  MEC callbacks (step 6). So **our game tap hears exactly what the TV hears, ducked or not**
  (VERIFIED). This corrects `research/AUDIO.md` 8.2 step 4, which calls 0x8012D158 a "master
  fade/mute state machine": it is the ducker (fields D+0x1A0..D+0x1C8 above, a per-sample gain
  ramp of 256 floats at `0x80189ED0` multiplied into all six channels).
- When does it duck (VERIFIED, XAM `0x81A44108`, XAM's per-frame render method, 187.5 calls a
  second): it measures the loudness of each local player's incoming chat (all four, before
  routing), and if any exceeds the threshold, holds "talking" for the hold time. It then calls
  `XAudioEnableDucker(talking AND some local player's speaker gain > 0)` every frame. A
  player's speaker gain is their Voice Volume when Voice Output includes Speakers, or when they
  have no headset, or their voice device is Kinect (`0x81A3F5A8`); otherwise 0. XAM also boosts
  its own speaker chat by 1 / level with the same attack and release, so the chat stays level
  while the game goes down.
- So: **with Voice Output = Headset and a headset present, Game Volume never acts** (VERIFIED for
  party chat; for game chat GUESS, as only XAM calls the ducker).
- Our hook and the ducker: `XamVoiceHeadsetPresent` goes through our `XVoicedHeadsetPresent`
  hook, which answers "present" while our pad's headset is served. XAM therefore does not fall
  back to the speakers. If our hook ever answered "no headset" (stage below 3, headset
  unplugged, a title switch), party chat would move to the TV speakers and the game would be
  ducked by Game Volume while someone talks (VERIFIED logic; not seen live).

## 3. Each setting against our panel today

| system setting | our panel | conflict? | what a player notices |
|---|---|---|---|
| Voice Volume | BALANCE (chat side), VOLUME | **Stacks** (chat = Voice Volume x BALANCE chat % x VOLUME). Same job as BALANCE's chat side, applied earlier. | At 10 (the owner): nothing. A friend whose profile has Voice Volume 3 hears quiet chat and pushes BALANCE to chat; both "work", but the panel cannot show why chat is quiet. Each profile can differ. |
| Game Volume | BALANCE (game side) | **Not touching** with Voice Output = Headset (the ducker stays off). With Speakers/Both or no headset it stacks: the game in the headset (and on the TV) drops 3 dB per step below 10 while someone talks. | At 6 with Headset: nothing. With Speakers: the game "pumps" down 12 dB in the headset whenever a friend speaks, on top of BALANCE. |
| Voice Output | the chat path itself | **Conflicts** unless Headset. Speakers: our chat path gets no chat, the voices come through the TV mix into our game tap (on BALANCE's game side, ~45 ms later, ducking-compensated). Both: chat in the headset twice, ~45 ms apart. | Speakers: chat on the TV, and faintly "in the game" in the headset; BALANCE toward chat makes it quieter. Both: an echo on every voice. |
| Voice Muted (profile) | mic mute (Share tap) | **Not touching**: ours silences the mic we send; theirs is a profile flag not on this page. | Nothing. |

## 4. Recommendations, ranked

The guiding fact: all three system settings act **before** our taps, and with Voice Output =
Headset two of them (Game Volume, Voice Output) are inert. So the least conflicting design keeps
the system's settings as the system's, makes the panel the only headset mixer, and only steps
in where a system setting would quietly break the headset.

### R1 (do first): read Voice Output and say so in the panel when it is not Headset

- What: on the file worker, beside `signin_job`, read `0x1004000D` and `0x1004000E` for the
  headset pad's user once when the headset binds, when that pad's profile changes, and when the
  panel is opened (at most once a second). If Voice Output is not Headset, the panel's help
  line (no pop-up) says "Chat is set to TV speakers: Guide > Settings > Preferences > Voice >
  Headset" (or "...both: you hear chat twice"). Also show it in a status block for `voice.py`.
- Calls: `XamUserReadProfileSettings(0xFFFE07D1, user, 0, NULL, 2, ids, &size, buf, NULL)` via
  `xa_kcall9`, synchronous (overlapped NULL), buffer sized once (two settings: about 0x60
  bytes; allocate 0x100 on the worker's stack or malloc at init).
- Risk: low. Same thread and same kind of read-only XAM question as `XamUserGetSigninInfo`.
  The read can wait on XAM's user lock (that is why it belongs on the worker, never on the
  driver thread, the XAM thread, a DPC or the render callback). Nine arguments: a direct call
  would put the overlapped pointer on the wrong stack slot (INTERNALS); with `xa_kcall9` it is
  the known path. Nobody signed in: XAM returns an error; show nothing.
- Work: small (about 80 lines in `settings_360.c` or a new small file, one status word, a
  help-line string, a `voice.py` field, an accept script).
- Test with the owner: set Speakers, then Both, then Headset in the Guide; the panel's help
  line follows within a second or two.

### R2: show Voice Volume, do not fight it

- What: from the same read, if Voice Volume is below 100, the BALANCE row's help line says
  "Voices also turned down in Guide voice settings (N/10)". The panel never compensates and
  never writes it.
- Why not compensate (scale chat by 100 / Voice Volume): it would undo a player's own choice
  and amplify a stream that was already scaled down (more noise, possible clipping of a
  re-scaled 8-bit / 16-bit packet). Why not write it from the panel: a profile write
  (`XamUserWriteProfileSettings`, xam 538) is a disk write and a profile sync, not instant, and
  whether a running game re-reads it is unknown (GUESS). The Guide page stays the one place.
- Risk: none beyond R1. Work: a few lines once R1 exists.

### R3: leave Game Volume alone; optionally show "not used with headset"

- With Headset it never acts, so it does not conflict with BALANCE. Mapping BALANCE onto it
  would be wrong: it only exists for chat through the TV.
- If wanted, the plugin can read it with no call at all: the ducker level's raw bits at
  `0x801960E8` match one of ten known values (step N = `10^(-3(10-N)/20)`), or manager+0x98.
  Writing it, if ever needed, is `XMsgInProcessCall(0xFD, 0x0010001B, step, 0)` exactly as the
  Guide does (live and saved); not recommended: it is a console-wide setting that belongs to
  the TV.
- Risk: reading is a plain load (safe on any thread, including the render callback); writing
  is an XAM call (file worker only; whether XAM accepts it from a system thread is GUESS).

### R4 (only if tests show ducking in the headset): undo the ducker in the game tap

- What: in the MEC callback, read the per-sample gain table at `0x80189ED0` (256 floats, the
  ramp the kernel just applied) or D+0x1AC (the last gain) as raw bits, and divide the tap's
  samples by it in integer arithmetic (ten fixed levels: a table of ten Q15 reciprocals, plus
  the ramp ends). The TV keeps the ducking; the headset gets BALANCE only.
- Risk: medium. Runs on the render thread at priority 25 (must stay short); boosting a ducked
  mix by up to 27 dB (step 1) raises its noise and can clip; reading kernel driver internals
  that `game_360.c`'s signature check does not cover yet (add the ducker body to the signature
  list). Work: medium (a day or two plus a host test).
- Only worth it if R1's warning is ignored in practice (players keep Speakers/Both) or if a
  game turns out to duck by itself.

### R5 (later, if Both is common): drop the TV copy of chat from the headset

- With Both, the headset gets chat from our chat path and again from the game tap. The game
  tap cannot separate chat from game sound, so the only clean fix is R1's warning; the
  alternative (mute our chat path when Both) loses the low-latency path. Not recommended.

Not recommended at all: making the panel rows write the system's own settings so "the Guide
and the panel always agree". Two of the three are per profile and slow to write, one is
console-wide and only matters for the TV, and none of them is a headset mixer: the panel would
end up moving the TV's behaviour from the headset's controls.

## 5. Threads and process context (Q6)

- Profile settings (`XamUserReadProfileSettings`): the **file worker** only, as `settings_360.c`
  already does for `XamUserGetSigninInfo`. Not the driver thread (a sign-in or profile load can
  hold XAM's user lock), not `xinput_360.c`'s XAM thread (it carries the Guide button and the
  binds), never a DPC or the MEC callback. Whether 17559's XAM answers a system-process caller
  for this export is GUESS; XAM's own engine and the Guide call it from XAM's process, and the
  Guide page reads it for a user index exactly as we would.
- `XMsgInProcessCall(0xFD, ...)`: file worker; it runs XAM's handler synchronously under the
  chat manager's critical section (`0x81902704`). GUESS that a system-thread caller is fine.
- Ducker fields and manager+0x98: plain 32-bit loads, any thread. They are written by the
  render thread (state, gain) and by XAM (level, requests); a torn read is impossible for an
  aligned word, a stale one harmless.
- No floating point anywhere: compare raw float bits, never convert.

## 6. Tests that need the owner (each with a friend talking, unless said)

1. In a party chat with the headset, move Voice Volume from 10 to 5 in the Guide: does the
   friend get quieter in the headset at once, and come back at 10? (We read `voice.py status`
   ear peak at the same time.)
2. In a game's own chat (Halo 3 over LAN), do the same: does the friend get quieter at once,
   only after leaving the match, or not at all?
3. Set Voice Output to Speakers: does the friend's voice leave the headset and come out of the
   TV, and can you still hear it faintly in the headset under the game?
4. Set Voice Output to Both: does every word sound doubled or echoey in the headset?
5. With Speakers set, while the friend talks: does the game get quieter on the TV and in the
   headset, and come back half a second after they stop? (We run
   `research/tools/dashvoice.py 30` and watch the ducker state change.)
6. Back on Headset, move Game Volume between 1 and 10 while the friend talks: confirm nothing
   changes in the headset or on the TV.
7. Signed out (no profile on the headset pad): confirm chat still plays in the headset.

## Appendix: addresses (17559)

Kernel: render driver D `0x80195F38`; ducker fields D+0x1A0 state, +0x1A4 on, +0x1A8 changed,
+0x1AC gain, +0x1B0 level, +0x1B4/+0x1B8 on/off requests, +0x1BC threshold, +0x1C0 attack,
+0x1C4 release, +0x1C8 hold; ducker step `0x8012D158` called at `0x8012E538`; gain ramp
`0x80189ED0`; exports 845-856 above.

XAM: voice settings ids `0x81A97E74`; default settings table `0x8162FA20`; per-player voice
engine: settings read `0x81A41A48` (re-read when its dirty flag +0xBC is set), speaker gain
`0x81A3F5A8` (+0xC4), volume gain +0xB8, flags +0xC8, headset present +0xC0; per-frame render
and ducking `0x81A44108` (vtable `0x8166D9A8`); render packets submitted at `0x81A4241C`
(`XamVoiceSubmitPacket`); chat manager global `0x81B2A368`, init `0x819028C8`, XConfig read
`0x81902800`, Game Volume setter `0x81901FE8`, message dispatcher `0x81901690`; export table
`0x81A73D34` (entries are RVAs from `0x815F0000`).

Guide (`hud.xex` at `0x913E0000`, copied from the console): voice page load `0x913F2A10`,
Voice Volume write `0x913F2790`, Game Volume write `0x913F2878`, change handler `0x913F2D58`;
control names `radgrpOutputLocation`, `sliderVolume`, `sliderDucking`, `chkMuteKinect`.
