# Session notes (kept by the orchestrator during the ROADMAP run)

## Console
- Nov 23, 02:25: after installing the `next` build with BASE-1 (image 0xcd000) and sending
  `magicboot cold`, the console did not come back (no ping either: it looks switched off, the
  known quirk of that command, but a boot failure of this build is **not ruled out**). The
  BASE-1 build is on its disk. A watcher on the Mac (`rescue_v3.py` in the session scratchpad)
  puts the published `wired-v3` build back on disk the moment xbdm answers, then logs whether
  the build that booted stayed alive for 2 minutes. No more installs until that is known.
  **For the owner: if the Xbox is off, switch it on. If it hangs at boot, power-cycle it once more
  (the second boot loads v3).**

- Nov 23, about 03:15: the Mac ran out of memory under five workers building at once and
  restarted. Lost: the watcher, the boot-risk review, and the uncommitted work of MICTEST v2
  and MIC-AUTO (plugin side). SETTINGS survived as uncommitted files in its worktree. Restarted
  at 10:20 with one Docker build at a time (`docs/WORKERS.md`); the watcher now lives in the
  new session's scratchpad. The console was still off at 10:20.
- Nov 23, 10:35: the console is back on. The BASE-1 build (image 0xcd000) **booted and stayed
  up**: xbdm answered 60 of 60 polls over 2 minutes, HUD block v11, stage 3, hooked, heartbeat
  running. So last night's no-show was the restart command switching the console off, not a
  boot failure. `wired-v3` is back on its disk (loads at the next boot); BASE-1 is in memory.
  Nothing newer than BASE-1 has run on a console yet.
- Nov 23, 10:45: **`next` at 0c7039e (release, image 0x85000) cold-booted cleanly**: xbdm 60 of
  60 polls over 2 minutes, HUD block v12, stage 3, VdSwap hooked at 22.2 s, XInput and voice
  hooks in, surround state allocated (gate off), driver loop normal. It is in memory and on
  disk (`wired-v3` is `xw.xex.prev`). Not yet exercised with a controller or headset.
- The independent boot-risk review of `next` found nothing serious. Fixed from it: `pe_va`
  took a log string for the XWMT block in XW_DEV builds; `tools/fn_diff.py` crashed on
  backslashes. Noted defaults that differ from v3 on purpose: PRESET row always shown (FLAT
  only), MUTED mark on, release builds skip `aud_analyse` (so `headset_test.py` needs XW_DEV),
  no XWTV block in default builds (`team.py` needs `XW_TEAM=1`).
- Nov 23, 11:45: **`next` at 879199e (release, image 0x88000, with mic test v2, saved settings,
  the file worker and mic auto) cold-booted cleanly**: xbdm 60 of 60 polls over 2 minutes, HUD
  v12 stage 3, all hooks in. New facts from this boot: the file worker starts on hardware
  thread 3 and **a plugin thread can read the HDD** (`xw.presets`: 29 presets loaded, CRC ok,
  6964 bytes). XWST and XWMA blocks present, every gate off. Three driver passes of 25 ms or
  more (max 34 ms) in the first 2 minutes, where the build before had none: watch whether
  they come from the preset load at boot or keep happening. It is in memory and on disk
  (`xw.presets` too). Not yet exercised with a controller or headset.

## Waiting for the owner (ears / hands)
- First: with `next` running, press the pad's Xbox button, plug in the headset, and check
  chat, sidetone, the Share panel, the test tone and the mic test behave as in v3.
- Then the per-feature checks listed under each section below.

## Contract deviations from ROADMAP as built (the headers are the source of truth)
- Mic test states: PAUSED = 3, READY = 4. Left while playing discards.
- Added `xq_resume`, `xs_resume`, `xf_init` / `xf_quiesce` / `xf_resume`; the `xf_post` queue
  holds at least 4 jobs.
- Settings record: `mic_level` is always the manual 1..10; AUTO is flag bit 0.
- `XQ_MAX_PRESETS` 64; file layouts are structs in `eq_360.h` and `settings_360.h`.
- The HRTF table struct lives in `surround_dsp.h` (SURROUND), not in a BASE-1 header.
- Gate AUTO (3) is accepted and behaves as LOW until MIC-AUTO lands; `xw.py` has no "auto"
  gate name yet; XD_VERSION stays 3.
- Release builds link `team_360_stub.c` (`XW_TEAM=1` for the real one); image 0xcd000.

## EQ (offline part merged into `next`)
- 29 presets, `xw.presets` 4672 bytes. Proven from a reverse-engineered A50 driver: gains are
  whole dB (-7..+7), bandwidth is in multiples of the centre frequency, bands 1 and 5 take no
  bandwidth. **Not proven:** that bands 1 and 5 are shelves (about 80 % sure) and the exact Q
  mapping; `BAND_SEMANTICS` in `tools/presets/convert.py` switches the reading.
- A preamp keeps boosts from clipping, which makes presets 5-13 dB quieter than FLAT.
  **the owner to judge** (and the PANEL/EQ work should consider level-matching FLAT or using the
  controller's amplifier headroom).

## Surround (offline part merged into `next`)
- HRTF source: SADIE II subject D1 (KU100 dummy head), University of York, Apache 2.0.
  **Release notes for any build that contains the table must carry the Apache notice and the
  dataset reference** (`tools/hrtf/CREDITS.txt`).
- Kernel input is s32 words holding s16 values (halfword sign-extending loads are microcoded
  on the Xenon), not s16 as the roadmap said.
- Estimated cost per 256-frame block: 64 taps 95-185 us (budget 300 us), 32 taps 50-95 us,
  128 taps likely too slow. To be measured on the console (roadmap 6.1, M3).
- With surround on, game sound is about 4.2 dB quieter than the plain mix (matching it would
  clip on full-scale 5-channel noise). `gen.py --match-loudness` makes the matched table.
- **For the owner, no console needed:** listen on headphones to
  `research/results/surround-demo/` (read `LISTEN.txt` there first): a sound walking around
  the head, and a scene as plain stereo vs virtual surround.

## Mic auto (offline part merged into `next`)
- Gate AUTO: floor tracker + 9 dB margin, clamp -60..-35 dBFS. On the owner's recording the LOW
  gate cuts 600 ms of his speech in 10 s; AUTO cuts none. In loud babble AUTO stays shut
  between words (LOW is open 99 % of the time) but missed 4 of 27 words; breaths open it.
- New 24 -> 8 kHz filter: at least 62 dB down above 4.6 kHz (today's path: 4.8 dB).
- MIC VOLUME AUTO: steady (no movement in pauses), settles in 1-7 s. Worker's opinion: good
  enough to offer as a choice, not as a default. Tuned on one talker's 10 seconds.
- **For the owner:** at MIC VOLUME 5 your voice reaches teammates about 13 dB below the usual
  voice-chat level (-30 dBFS); AUTO shows it as level 9. Worth a listen before deciding the
  default mic volume.
- Contract: `mic_gain_target()` needs a pad argument (AGC gain is per player): BASE-2 /
  MIC-AUTO plugin phase.
- The comb canceller is not shipped.

## EQ (plugin part merged into `next`, gate off by default)
- Loudness is matched: every preset plays at the level of the quietest one, and FLAT plays
  5.7 dB quieter **while the EQ gate is on and a preset file is loaded**, so switching never
  jumps and nothing can clip. With the gate off (today's default) nothing changes.
  **the owner to judge:** turn VOLUME up a step to make up the 5.7 dB; is that acceptable?
- Code is 5.1 KB against a 4 KB budget (accepted by the orchestrator: the filter loop is
  already the only part built for speed). Heap 7 KB. Estimated 45-135 us per earpiece block.
- `eq_360.h`: the file header's reserved word is `flat_q15` (FLAT's level).
- To do by others: BASE-2 calls `xq_init` after `xf_init`; SETTINGS applies the saved preset
  only once `xq_count()` > 0 (the file loads in the background); orchestrator adds
  `out/xw.presets` to `xw.py install` and the EQ knobs to `knob_specs`; the EQ's local
  barrier macro moves to `kernel_360.h`.

## Panel v4 (merged into `next`)
- New rows, display order, AUTO values, record / play / pause mic test, the MUTED mark
  (XWPN `badge` default on, `badge_mode` 1 = 2 s of every 15; position XH36 `badge_pos`).
  XWPN is v3 (42 words); XH36 is **v12** (one word more than the contract: `badge_idle_ms`).
- Canvases are malloc'd: release image 0xbc000 (was 0xcd000).
- `panel_360.c` has stand-ins returning false for getters the features must provide:
  `xg_surround_offered` (SURROUND), `xd_mic_auto_offered` / `xd_gate_auto_offered`
  (MIC-AUTO), `xt_offered` (team). Each feature replaces its stand-in at integration.
- Existing quirk, not fixed: letting go of Share before 400 ms toggles mute even if the
  panel had already appeared (hold 250 ms < tap 400 ms). To fix in the wrap-up wave.
- Previews: `research/results/panel-v4/*.png` (rendered on the Mac, not console screenshots).
- **For the owner:** is the MUTED mark steady and out of the way in Halo 3, and which corner?
  Can every row be read? Does record / play / pause / discard feel right?

## Surround engine (merged into `next`, XWSR gate off by default)
- Design A: the virtualizer runs in the audio tap when SURROUND is on. With the gate off the
  tap is the old path plus one load and one branch; host tests prove gate-off / surround-off
  output equals the old downmix exactly and surround-on is bit-exact with the Python model.
- Loudness-matched table (no level drop when switching on). Only full-scale white noise on
  all five channels clamps (0.3 % of samples).
- `g_ring` (32 KB) is now allocated at init: **a boot-path change in the default build, not
  yet run on the console.** Release image 0xc6000 before the panel's saving is counted in.
- Heap 14.8 KB + the ring. Table magic is `XWHR`; functions use the `xsv_` prefix.
- 32 taps needs `XW_SURROUND_TAPS` plumbed through the two build.sh files (orchestrator).
- **Release notes must carry:** "This build contains headphone-surround filters derived from
  the SADIE II database (subject D1, KU100), (c) 2018 University of York, used under the
  Apache License 2.0; see NOTICE and tools/hrtf/CREDITS.txt."
- **For the owner (console):** `accept/surround_cost.py` on an XW_DEV build during a Halo 3
  match (does anything crackle?), then `accept/surround.py --hands`: a sound walks around
  your head, say where it is; play with SURROUND on and off.

## BASE-2: split + fence + pre-wire (merged into `next`)
- `voice_360.c` -> `voice_360.c`, `side_360.c`, `micdsp_360.c`, `mictest_360.c` + `voice_priv.h`;
  `xw_plugin.c` -> + `reload_360.c`, `selftest_360.c` + `xw_priv.h`; `dsp_360.h`, `kernel_360.h`.
- Release image with everything merged so far: **0x84000** (start of the night: 0xde000).
  XW_DEV build: 0x8a000.
- Proof: 258 function bodies byte-identical in source; at -O0, 511 functions identical and
  11 changed, all named (the pre-wired calls and the two heap moves). **At -O2 the machine
  code is NOT identical**: 23 functions changed and 35 are no longer inlined, because callers
  moved to other files, and global data is laid out differently. So behaviour is preserved
  by construction, but timing is not proven: **the console regression run decides.**
- Boot-path changes, most suspicious first: `XwThreadC` (new init calls, take-over code now
  a call), 192 KB less static data with globals regrouped, `rx_ring` malloc'd before first
  use, release `XwLinkDpcC` no longer traces.
- Release builds refuse dev-only tool commands with "needs an XW_DEV build" (reload, trace,
  gate, micdump, usbreset, mecreset, hsrestart, audio 5, tone, diag probe).
- Open: `size_report.py` does not pass XW_DEV / XW_TEAM into its map rebuild; `accept/wp1_4.py`
  writes the probe word directly and needs a dev check; `game_360.c` / `team_360.c` keep their
  own `clamp16` (different type); `reload_360.c` is 730 lines (pure move).
- **For the owner (console):** does chat, sidetone, the test tone and the mic test sound exactly
  as before?

## Mic test v2 (merged into `next`)
- Record / play / pause / resume / play again / discard; the clip goes when the panel closes
  with nothing running. Buffer 1.44 MB (30 s), fallback 480 KB (10 s), freed on every return
  to IDLE. XWMT is v2, 41 words; `secs` counts up while recording.
- Host test 112 checks in release and XW_DEV; image 0x85000 (code +3.0 KB).
- UNVERIFIED on the console: whether 1.44 MB is free during a game (XWMT `fallback`,
  `alloc_fails`, `alloc_us` show it) and what freeing it inside the earpiece mix costs.
- Stale comment for PANEL to delete: `hud_360.h` mictest_secs "seconds LEFT". When MIC-AUTO
  adds the pad argument to `mic_gain_target`, the fake in `tests/test_mictest.c` needs it too.
- **For the owner:** hold Share, go to MIC TEST, press Right and talk for as long as you like,
  press Right to stop and hear it, Right to pause, Right to carry on, let it finish and press
  Right to hear it again; let go of Share and open the panel again: the clip is gone. Then
  `accept/mictest.py` once in a Halo 3 match (is 30 s of memory available there?).

## Saved settings + file layer (merged into `next`, XWST gate off by default)
- `file_360.c`: a low-priority worker thread (hardware thread 3, 32 KB stack) does all file
  work; `settings_360.c`: 64 records by XUID in one CRC'd file, saved 2 s after the last
  change, restored at boot / headset bind / sign-in change. `settings.py`, `accept/settings.py`.
- The saved preset is applied only once `xq_count()` > 0; a failed preset load looks like a
  slow one (no "load failed" getter in `eq_360.h`), so the id is kept and re-checked.
- Code is about 11 KB against a 6 KB budget (accepted by the orchestrator: the image is
  0x87000 with mic test v2, against 0xde000 at the start and a 0x100000 limit).
- **Boot-path change with every gate off:** `xf_init` creates the worker thread at every boot
  and `xq_init` reads `xw.presets` on it: the plugin's first file I/O from its own thread.
  Cold-boot test with the v3 fallback before the owner boots it.
- The sign-in question (`XamUserGetSigninInfo`, ordinal 551) runs on the file worker, not the
  XAM thread: `docs/DEVELOPMENT.md` "XAM only from the XAM thread" needs that exception.
- A release build loses a change made in the last 2 s before power-off (no quiesce there).
- UNVERIFIED on the console: ordinal 551 and its struct (from Xenia's table), XAM answering a
  system thread, the "no such user" code, writing and rename-with-replace from a plugin
  (reads on `\Device\Harddisk0\Partition1` are proven), save latency and earpiece starvation.
- **For the owner:** change VOLUME and BALANCE, restart the console, check they stayed. Sign in
  as a second profile: it starts from the normal settings and keeps its own.

## Mic auto (plugin part merged into `next`; XWMA switches all off by default)
- Gate AUTO, MIC VOLUME AUTO and the 24 -> 8 kHz FIR in `micdsp_360.c` behind block XWMA (20
  words: `gate_auto`, `mic_auto`, `fir`, `comb` under XW_DEV). 1120 bytes of heap only while a
  switch is on. XWDG is v4, same layout. `micauto.py`, `accept/micauto.py`.
- Proof: 40 manual gate / volume combinations give output and XWDG numbers identical to the
  code before the change; the model scenarios are bit-exact through the real file.
- Integrated by the orchestrator: `mic_gain_target(pad)` everywhere (the compatibility macro is
  gone), the panel's AUTO values follow `xd_mic_auto_offered` / `xd_gate_auto_offered`.
- `fir` default is 0. Worker recommends 1 once `accept/micauto.py` step 5 passes: aliasing
  above 4.6 kHz -4.8 dB -> -62 dB, level +0.15..0.30 dB, flat at 3.2 kHz (old: -2.2 dB),
  delay +0.88 ms.
- Images with everything merged: release 0x88000, XW_DEV 0x8f000, XW_TEAM 0x9b000.
- To do in the wrap-up: XWMA switches in `xw.py` `knob_specs`; XV36 `mic_peak` does not move
  while the FIR has the stream; the mic test records before the FIR, so it cannot demo it.
- **For the owner:** gate on AUTO: talk, go quiet, does the gate dot go out? MIC VOLUME AUTO:
  count to ten, does the level move and settle? A friend on another console listens while
  the filter switches A / B: which is clearer?

## Panel 5: preset description and CUSTOM rows (merged into `next`)
- Description at the very bottom, under the help line, wrapped at 36 characters (at most 3
  lines for 63 characters, proven); the area's height is fixed per Share press from the
  file's longest description, so the panel does not jump while stepping presets.
- CUSTOM is the last PRESET value; BASS / MIDS / TREBLE rows appear under PRESET while it is
  selected. `hud_state` v5, XH36 v13, XWPN v4 (`features_force` 0x20 / 0x40 / 0x80).
- Proven in `tests/test_panel.c` (47156 checks): every row plus a 3-line description fits
  480p, 720p, 1080p and 1152x640; with nothing offered the 48 old pictures are identical.
- Release image 0x89000; canvases 74.9 -> 104.6 KB at boot; the largest panel repaints 42 %
  more pixels. Draw time and the larger boot allocation are not measured on the console.
- **For the owner (console):** is the description readable at the bottom; do the BASS / MIDS /
  TREBLE bars make sense; does holding Left / Right on them feel right?

## EQ-2: descriptions, the release preset set, CUSTOM (merged into `next`)
- Preset file v2 (224-byte records with a description; CUSTOM tables after them). **A v1 file
  is refused (FLAT only)**: install `xw.presets` with every build from here on.
- 18 presets + CUSTOM: HALO 2, HALO 3 (ours, first guesses made without listening:
  `tools/presets/xw/*.xweq`, one number per tweak), HALO MCC, HALO INFINITE, OMNIVOX, ARCTURUS,
  DYSTRILATION, PURE, MOVIE: INCENDIARY, TOURNAMENT II / III, VOICES, BASS BOOST, LATE NIGHT
  (ours), REVIVE A40 GEN3 / GEN4, STEPS ONLY, STEPS WIDE. Dropped upstream files are listed as
  unused in `names.txt`. A saved choice of a dropped preset loads as FLAT.
- CUSTOM: BASS shelf 200 Hz, MIDS bell 1 kHz, TREBLE shelf 4 kHz, 1.5 dB a step, preamp
  tabled for all 2197 combinations; saved per profile in spare record bytes (settings file
  version unchanged). XWEQ v2, 23 words. EQ heap 12.9 KB (budget raised to 16 KB).
- Release image 0x8a000 with panel 5.
- **For the owner:** HALO 2 / HALO 3 against FLAT in a match (steps and reloads easier to hear
  and place? a grenade landing next to a Warthog? harsh after an hour? announcer thin?); hold
  each CUSTOM control end to end, any clicks?; does VOLUME make up for the 5.7 dB?

## EQ closed by the owner (Nov 23)
- Final preset set, replacing the EQ-2 list above: FLAT, CUSTOM, HALO MCC, OMNIVOX, ARCTURUS,
  DYSTRILATION, PURE, MOVIE: INCENDIARY, BASS BOOST, REVIVE A40 GEN3, REVIVE A40 GEN4. The
  PRESET row walks FLAT, CUSTOM, then the file's presets (`panel_360.c` `preset_pos`); the
  engine's indexes are unchanged (CUSTOM after the file's presets). HALO 2 / HALO 3 (guesses
  made without measurements), VOICES, LATE NIGHT and the STEPS filters were dropped.
- Panel 6 replaces panel 5's description area: the preset's description (36 characters at
  most) is the help line while the cursor is on PRESET. XH36 v14, XWPN v5.
- The driver thread only gets about 1 ms a frame (the system window, `docs/INTERNALS.md`).
  A 5-band preset used to lose every third pass to it. Now a stopped pass is followed at
  once by the next, and `win_boost` (default **on**) lets a long pass finish at priority 20.
  Measured in a Halo 3 match on DYSTRILATION: boost off 8 passes a second stopped, none of
  25 ms, none starved; boost on 0 stopped in 20 s. EQ time about 350 us a pass for 5 bands.
- Rumble honours the profile's Vibration setting (XAM's scale byte), stops are sent twice,
  motors stop on a title change; block XWRB, `rumble.py`, `accept/rumble.py`.

## Nov 23 2026 (evening): gate steps, surround demo, shipped defaults

- Noise gate: AUTO plus four manual steps (-60/-55/-50/-45 dBFS). The owner settled on step 3
  after AUTO let background noise into Halo 3 chat; step 3 is now the default.
- Surround: judged good by the owner in Halo 3 ODST (5.1 live, rear channels at -20 dBFS). The
  tap measured 419 us a block (budget 300 us) with no audible cost. SURROUND DEMO row plays
  `Hdd:\Plugins\xw.demo` (`python3 tools/demo/make_demo.py`).
- Shipped defaults are the owner's panel on the SCUF Instinct Pro: VOLUME 8, SIDETONE 7, MIC
  VOLUME 6, NOISE GATE 3, equalizer on with HALO MCC, SURROUND on. Booted and read back on
  the console (b1874d6).
- Open: saved settings across a restart, auto mic volume, chat tests with a second player, a
  restart that never powers the console off, a PC bridge for music and Discord (idea).
