# HANDOFF: headset work on xw.xex (written Nov 23 2026, beta 1)

For the agent taking this project over. Read this first, then `CLAUDE.md`, `docs/INTERNALS.md`
and `docs/PLAN.md` (the phase plan; this file adds a new block of work, section 4).

## 1. What this is

`xw.xex` is a plugin for the owner's RGH Xbox 360 that makes wired Xbox One / Series controllers
work, and turns a headset plugged into the controller's jack into a replacement for an Astro
MixAmp: game audio + chat in the earpiece, balance, volume, sidetone, mic gate, an on-screen
panel on the Share button. Goal: use it at the owner's next LAN instead of MixAmps, for
non-technical friends. "Idiot proof" is the design rule.

## 2. Where everything is

- GitHub (private): `git@github.com:Roasted-Codes/Xbox360-GIP-Wired.git`, one branch: **`main`**
  (tag `beta-1` = the state described here). On the owner's Mac the repo is
  `/Users/owner/Documents/code/Xbox360-GIP-Wired`; there the GitHub remote is called `private`
  and `origin` is the upstream project (Jacckii): never push to upstream.
- The development history had about 40 feature / experiment branches. They were merged or
  dropped on Nov 23 2026; a full backup of every branch is on the owner's Mac at
  `/Users/owner/Documents/code/_archive/Xbox360-GIP-Wired-all-branches-2026-11-23.bundle`
  (`git clone` it to look). Older docs mention branch names like `headset-audio`, `sidetone`,
  `mictest`: all of that is in `main` now.
- Not in git (ignored): `xbox360/out/` builds and console logs; rebuild with the toolchain.
- Build: `bash toolchain/build.sh` (Docker). Must print `out/xw.xex: image 0x..... bytes, ok,
  no FP`. **Image size limit: under 0x100000; it is 0xdd000 now (about 140 KB left).** Big
  tables and buffers must be `malloc`'d or loaded from a file at run time, never static.
- Kernel rules (INTERNALS.md): no floating point, no .bss, thunks for kernel callbacks, Usbd
  only on its DPC, kernel objects as full-size structs.
- Console: `10.0.0.13`, xbdm port 730, plugin at `Hdd:\Plugins\xw.xex`. All tools:
  `cd xbox360; export XBOX=10.0.0.13`.
  - `python3 xw.py status|diag|tee`, `voice.py status`, `hud.py status`.
  - Install + reboot (the owner has OK'd doing this without asking):
    ```
    python3 -c "
    from xa import Xbdm
    P='Hdd:\\\\Plugins\\\\'
    Xbdm().sendfile(P+'xw.xex.prev', Xbdm().getfile(P+'xw.xex'))
    new=open('out/xw.xex','rb').read()
    print(Xbdm().sendfile(P+'xw.xex', new), Xbdm().getfile(P+'xw.xex')==new)
    print(Xbdm().cmd('magicboot cold'))"
    ```
  - `magicboot cold` sometimes switches the console **off** instead of rebooting. If it is not
    back in ~2 minutes, ask the owner to power it on; do not assume the build is bad.
  - After a cold boot the controller is dark until the owner presses its Xbox button; no headset
    data until then.
  - Nothing needs switching on after a boot: the panel (hud stage 3, draw thread on hardware
    thread 1 at priority 15) and the MIC TEST row are compiled defaults since Nov 23 2026.
    The HUD waits 20 s after start before it hooks anything (`BOOT_HOLD_MS`, hud_360.c): a
    bad build can be replaced over xbdm in that window, and `hud.py stage 0` keeps it off.
  - `shot.py` decodes Halo 3's front buffer wrongly (xbdm's header says 8_8_8_8, the buffer
    is 2_10_10_10: decode with the format from `hud.py status`), and `--find` no longer
    matches the panel's palette. Look at the PNG instead.
  - Acceptance scripts: `xbox360/accept/*.py` (`mictest.py`, `sidetone.py`, `wp1_3.py`, ...).
    Each prints PASS/FAIL lines; `--hands` adds y/n checks for the owner.
- Test on the console only. Never hand the owner Mac-side previews or files to review.
- Talking to the owner: plain language, no jargon, short. He is not an audio engineer. Ask him to
  listen and report; do the measuring yourself.

## 3. State on the console right now (build 83461ff; later commits are docs only)

Working and checked:
- Wired pad, headset audio both ways, game audio in the earpiece (kernel tap: 256 frames x 6
  channels float32 at 48 kHz, downmixed to stereo in `game_360.c`).
- On-screen panel (hold Share): BALANCE, VOLUME, SIDETONE, MIC VOLUME, NOISE GATE, TEST TONE,
  MIC TEST, with a help line per row. Share tap = mute.
- Sidetone by the headset adapter's own DSP (no delay), via the GIP volume message.
- MIC TEST: Right records 10 s (what teammates hear), plays back once, game ducked to 25 %.
  `accept/mictest.py`: 28/28 passed.
- MIC VOLUME: 10 steps of 3 dB (25..560 %, 5 = 100 %); the mic's DC offset (+60) is removed;
  meter rescaled so speech sits mid-scale.
- **VOLUME now turns the controller's own amplifier** (`dev_vol`, XVHS tail; `voice.py
  sidetone devvol 0|1`). Verified in the block: VOLUME 5 -> amp 50 %, headset reports 50,
  samples at full level. the owner has not yet said how the steps feel; if the low end is too
  quiet, add a curve in `xv_headset_volume` (voice_360.c).

Findings from the "buzz" investigation (data in `research/results/2026-11-23-buzz/`, scripts
in `research/tools/`, need numpy/scipy/matplotlib):
- The buzz the owner hears is the **controller's headphone amplifier's own noise** (audible with
  digital silence going out, mic switched off, sidetone 0; dropped when the amp was turned
  down). Probably USB power noise from the 360. Hence the VOLUME change above. A different USB
  port or a USB isolator would confirm; not tried.
- With sidetone up and the mic switched off he hears the same noise through the adapter's mic
  input: hardware, nothing to fix in software.
- Mic: speech about -40 dBFS, noise between words about -64 dBFS (only ~22 dB below). The
  headset's own mic gain (GIP volume message, `devmic`) at 50 lowered speech and noise alike;
  leave it at 100.
- A faint 125 Hz-spaced comb (8 ms, the USB block period, exactly 192 samples at 24 kHz) is in
  the raw mic at about -75 dBFS. A 192-sample running-average canceller removes it offline
  (`research/tools/cancel.py`). Low priority.
- Known quality bug: mic 24 kHz -> chat 8 kHz uses a 3-tap box + linear interpolation
  (`rs_push`, voice_360.c): aliasing. Needs a real low-pass (integer FIR) before decimation.

### Defaults (the owner's own settings, noted Nov 23 2026 on the SCUF Instinct Pro)
BALANCE even, VOLUME 5, SIDETONE 5, MIC VOLUME 5, NOISE GATE AUTO (manual steps 1..4 at -60 -55 -50 -45 dBFS; the old LOW -45 cut his words), TEST TONE off; hiss control
on (`dev_quiet` 3), VOLUME by the controller's amplifier (`dev_vol` 1), headset mic gain 100,
game buffer 45 ms. These are the compiled defaults from branch `auto-quiet` on.

## 4. The work the owner has asked for (agreed Nov 23 2026), in order

General rules for all of it: every new control gets an xbdm status block field and an
`accept/` script; append-only blocks, bump versions, keep `voice.py` / `xw.py` / `hud.py`
mirrors in step; one console reboot per batch.

### 4.0 Panel on at boot: DONE (Nov 23 2026)
Verified with a cold boot and no commands: stage 3, VdSwap hooked 22.8 s after start, draw
thread on hardware thread 1 at priority 15, MIC TEST row shown, panel drawn in Halo 3. The
compiled `xw.xex` is attached to the GitHub release `wired-v1`. Still open: a way to keep the
panel off without a computer if a title misbehaves (e.g. a file `Hdd:\Plugins\xw.safe`).

### 4.1 MIC TEST: any length, replayable
- Right = start recording; Right again = stop (auto-stop at 30 s). It plays once
  automatically. Then the row shows READY with a clip: Right = play again (any number of
  times), Left = discard (back to empty). Left while recording/playing = stop.
- Buffer: 30 s x 24 kHz x 2 bytes = 1.44 MB, `malloc` on first use (today 480 KB). Check the
  allocation succeeds on the console with a game running; fall back to 10 s if not.
- `hud_state` has 1 reserved byte left; it needs a "clip held" state (extend `mictest` enum:
  idle-empty, recording, playing, idle-with-clip) and seconds as a count-up.
- Files: `voice_360.c` (mt_*), `diag_360.h`, `panel_360.c` (mictest_adjust), `hud_panel.c`,
  `voice.py`, `accept/mictest.py`.

### 4.2 Settings saved per player profile
- Save: balance, volume, sidetone, mic volume (or AUTO), gate mode, preset/EQ choice (4.4),
  surround on/off (4.5), mute is not saved.
- Key: the XUID of the profile signed in on that pad (XamUserGetXUID); a `default` record
  when nobody is signed in. File: `Hdd:\Plugins\xw.settings` (small fixed-size records,
  magic + version + checksum). `xa_plugin.c` already does NtCreateFile-style file I/O: reuse.
- Write only from the driver thread, debounced (e.g. 2 s after the last change), never from a
  DPC or hook. Load at headset bind and on sign-in change.
- PLAN.md WP3.4 (settings survive hot reload) can share the same struct.
- Acceptance: change three settings, cold reboot, all three back; sign in a second profile,
  it gets its own.

### 4.3 Automatic noise gate and mic volume ("just know")
- NOISE GATE becomes **AUTO (default) / OFF**. AUTO: track the noise floor (minimum-statistics
  over a few seconds of the 20 ms RMS windows, after DC removal), threshold = floor + ~8 dB,
  clamped to about -60..-35 dBFS, keep the 200 ms hold and 10 ms ramps. Publish floor and
  threshold in XWDG.
- MIC VOLUME gets **AUTO (default)** plus 1..10. AUTO: slow AGC toward speech at about
  -30 dBFS RMS (measured on gate-open windows only), limited to the existing 25..560 % range,
  slew-limited (about 1 dB/s), frozen while the gate is shut. Optionally seed it from a MIC
  TEST recording (it already measures `rec_rms`).
- Also here: the proper 24 -> 8 kHz low-pass, and optionally the 192-sample comb canceller.
- Later, bigger: noise suppression (spectral, integer-only) for the hiss under the voice.
  Not agreed as part of this block; the owner was told it is the biggest win for teammates.

### 4.4 Equalizer and headset presets (TheZEFERENCE)
- A 5-band parametric EQ on the earpiece output (after the mix, before the amp), integer
  biquads (Q2.30 or similar, 64-bit accumulators, per channel), with headroom: apply a
  negative preamp equal to the largest boost, clamp.
- Presets: https://github.com/XxUnkn0wnxX/TheZEFERENCE (`.astroeq`, INI text: `Eq_Band_1..5`
  gains, `Eq_Freq_Band_1..5` Hz, `Eq_Bandwidth_2..4`; bands 1 and 5 have no bandwidth,
  presumably shelves; units assumed dB / Hz / octaves: **verify against Astro Command Center
  behaviour before trusting**). Astro A40/A50 only; nothing for SteelSeries. **the owner says
  bundling these files is cleared.** The repo itself shows no license: keep the credit to
  ZaliaS / TheZEFERENCE in the bundle.
- Convert presets to biquad coefficients **on the Mac** (a Python script, FP allowed there)
  into a table file installed next to the plugin (`Hdd:\Plugins\xw.presets`); the plugin only
  loads coefficients. Keeps FP and size out of the image.
- Panel: a **PRESET** row (FLAT, then the bundled names; scroll with Left/Right). The
  controller cannot identify an analog headset, so the player picks once; saved with their
  profile (4.2). The presets were tuned for a MixAmp's output, so expect differences.
- Shelved by the owner for later: players submitting presets to a shared database.

### 4.5 5.1 surround in the headset
- No Dolby/DTS code is needed, and `dcaenc` (a DTS *encoder*) is the wrong direction: the tap
  already delivers the six PCM channels (FL FR C LFE SL SR). Replace the plain downmix with a
  binaural virtualizer: each of the 5 main channels convolved with a left-ear and right-ear
  impulse response for its speaker position (open HRTF sets: MIT KEMAR, SADIE II; check the
  license of the one chosen), LFE added to both ears, optional short room tail.
- Integer only: Q15 FIR, start with ~64-128 taps at 48 kHz, 10 convolutions (use left/right
  symmetry to halve the table). Coefficients from a file, as in 4.4.
- **First step is a measurement, not code:** the cost. 10 x 128 taps x 48000/s is about
  61 M multiply-adds a second on one hardware thread without VMX (no FP rule: check whether
  VMX integer ops are allowed under INTERNALS.md; if so use them). Use XWDG `work_us` /
  `pass_us` to measure a dummy load of that size while a game runs, and watch
  `out_frames_missed` and game underruns. If too slow: fewer taps, or process on the tap's
  render-thread side at 256-frame blocks.
- Check that the 360 really sends 5.1 into the tap (console audio set to Dolby Digital 5.1;
  a stereo title gives two channels and should bypass the virtualizer).
- Panel: SURROUND ON/OFF row, saved per profile. EQ (4.4) runs after it.

## 5. Other open items (from PLAN.md and this session)

- Hot reload works only with `--in-place`, and headset audio on a reloaded build froze the
  console (PLAN.md Phase 3). Until fixed, every change needs a reboot.
- SCUF Instinct Pro on the console: PLAN.md Phase 2.
- Team voice between consoles: PLAN.md Phase 4. The `wp4.0` / `wp4.2` code is merged, but
  whether it works on the console was not checked by this session (a `bisect-no-wp4.2`
  branch exists, so it was suspected of something once).
- `CLAUDE.md` in this repo has a long status section; add a short entry per finished item.
