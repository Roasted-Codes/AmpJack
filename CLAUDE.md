# AmpJack (repo folder: Xbox360-GIP-Wired)

A plugin (`xw.xex`) for a modded Xbox 360 (RGH, kernel 17559) that makes **wired** Xbox One /
Series controllers work, and turns a headset in the controller's jack into a MixAmp
replacement with an on-screen panel on the Share button. Fork of
[Jacckii/Xbox360-Series-Controllers-Native](https://github.com/Jacckii/Xbox360-Series-Controllers-Native)
(GPL-3.0); the original wireless plugin (`xa.xex`) still builds from this tree.

`main` is the verified state. Compiled builds are on the GitHub releases page.

## Read first

1. `docs/HANDOFF.md`: what works today, how to drive the console, the agreed next work.
2. `docs/INTERNALS.md`: the kernel rules. Breaking one freezes the console.
3. `docs/PLAN.md`: the roadmap. `docs/WIRED.md`: install and design.
   `research/AUDIO.md`, `research/HUD.md`: how the audio and the panel were worked out.
4. `docs/WIRELESS-NOTES.md`: what adding wireless pads to `xw.xex` would take (untested).

## Layout

- `core/`: the GIP controller protocol, shared by every target.
- `xbox360/`: the plugin. `xw_plugin.c` (USB driver, main loop), `voice_360.c` (the 360's
  voice hooks), `side_360.c` (sidetone, the headset's own volume), `micdsp_360.c` (mic
  processing, diagnostics block), `mictest_360.c`, `game_360.c` + `surround_360.c` (game
  audio), `eq_360.c`, `settings_360.c` + `file_360.c`, `panel_360.c` (Share gestures and
  rows), `hud_360.c` / `hud_panel.c` (drawing the panel). `docs/DEVELOPMENT.md` has the map.
- `xbox360/*.py`: Mac-side tools that talk to the console: `xw.py`, `voice.py`, `hud.py`.
- `xbox360/accept/`: acceptance scripts, one per feature, PASS / FAIL lines.
- `desktop/xwired.c`: the same controller code as a Mac / Linux test tool.
- `toolchain/`: Docker build. `research/`: notes, measurements, analysis scripts.

## Build and install

- `bash toolchain/build.sh`. It must print `out/xw.xex: image 0x... bytes, ok, no FP`.
- The image must stay under 0x100000 bytes (0x8d000 now). Big tables and buffers are
  `malloc`'d or loaded from a file, never static.
- Console: `10.0.0.13`, xbdm port 730, `Hdd:\Plugins\xw.xex`. Install and reboot:
  `docs/HANDOFF.md` section 2. Everything is on at boot; no commands needed afterwards.
- Releases: attach `xbox360/out/xw.xex` to a GitHub release (`gh release create`) after it
  has booted cleanly on the console with no computer attached.

## Rules

- Kernel code: no floating point, no .bss, a thunk for every kernel callback, Usbd only on
  its DPC, kernel objects as full-size structs.
- Never load `xw.xex` together with `xa.xex` or hiddriver360 (same XInput hooks).
- Status blocks read over xbdm are append-only and versioned; keep the Python mirrors in step.
- Every feature gets an `xbox360/accept/` script and is verified on the console, not on the
  Mac.
- Don't run `desktop/xwired` with audio on macOS 26.6.2 (25G83): it panics the Mac.
  `XWIRED_NOAUDIO=1` is safe.
- Git: every feature on its own branch; merge to `main` only once verified on the console,
  then delete the branch. `docs/ROADMAP.md` is the current plan (integration branch `next`). Push to the private remote
  only, never to upstream (Jacckii).

## Working with the owner

- Plain language, short, no jargon. He listens and reports; you measure.
- Everything should "just work" for non-technical friends at a LAN. Controls must be instant,
  with no pop-ups.
- Installing builds and rebooting the 360 needs no permission. After a cold boot the
  controller stays dark until he presses its Xbox button.

## Hardware verified

- Microsoft Xbox Series pad (045e:0b12) with a headset in its jack (045e:02f6): on the console.
- SCUF Instinct Pro (2e95:0504) with a headset in its jack: on the console (Nov 2026), the
  owner's daily pad; the shipped panel defaults are his settings on it.
