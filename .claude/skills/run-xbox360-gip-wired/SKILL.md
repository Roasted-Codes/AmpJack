---
name: run-xbox360-gip-wired
description: Build, install, boot, run, drive, screenshot and test the xw.xex plugin on the real Xbox 360 over xbdm. Use to try a new build safely, open the Share panel without hands, take a screenshot of the console, check the audio loop for stalls, or run the host tests.
---

# Run AmpJack

`xw.xex` is a kernel plugin that only runs on the modded Xbox 360 at `10.0.0.13` (xbdm port
730). There is no emulator: "running it" means building in Docker on the Mac, copying it to
`Hdd:\Plugins\xw.xex`, cold-booting the console, and driving it over xbdm with
`.claude/skills/run-xbox360-gip-wired/driver.py`. All paths are relative to the repo root.

## Prerequisites (after a Mac restart)

```bash
open -ga Docker                 # the build needs Docker Desktop running
ssh-add --apple-load-keychain   # or git push to the private remote fails with "Permission denied (publickey)"
```

## Build

One Docker build at a time on this Mac (five at once ran it out of memory):

```bash
lockf -t 3600 /tmp/xw-build.lock bash toolchain/build.sh
lockf -t 3600 /tmp/xw-build.lock env XW_DEV=1 bash toolchain/build.sh   # development build
```

It must end with `out/xw.xex: image 0x... bytes, ok, no FP`. Every configuration writes the
same `xbox360/out/xw.xex`: rebuild the release one last before installing. The equalizer needs
its preset file next to it (`xbox360/out/xw.presets`; a version the build does not know means
FLAT only):

```bash
python3 tools/presets/convert.py --vectors -q
```

The surround demo (SURROUND DEMO row, `surround.py demo start`) needs its clip next to it,
`xbox360/out/xw.demo` (7.9 MB, not in git; about 7 s to make):

```bash
python3 tools/demo/make_demo.py
```

## Run (agent path)

```bash
export XBOX=10.0.0.13
D=.claude/skills/run-xbox360-gip-wired/driver.py
python3 $D up 5            # is the console answering?
python3 $D status          # title, panel stage, loop passes, starved audio blocks, EQ
python3 $D shot /tmp/screen.png
python3 $D panel /tmp/panel.png down 5 right 1   # hold Share, walk to PRESET, step it, screenshot
python3 $D slow 6          # driver-loop passes of 25 ms or more in 6 s (0 is healthy)
python3 $D keep            # out/xw.xex + out/xw.presets + out/xw.demo onto the disk, no reboot
```

Look at the PNG you took. `panel` changes what it presses (the example selects preset 1):
put it back, e.g. `(cd xbox360 && python3 eq.py select 0)`.

Feature switches are off at every boot until their defaults are flipped; switch on what you
are testing, from `xbox360/`: `python3 eq.py on`, `python3 settings.py on`,
`python3 surround.py gate on`, `python3 micauto.py gate on`. Each tool prints its commands
with `help`.

### Booting a build that has never booted

Never install an untested build and walk away: a build that freezes at boot stays on the disk
and freezes every boot. Get the last published build as the fallback, then:

```bash
gh release download v1 -R Roasted-Codes/AmpJack -p xw.xex -D /tmp/v3 --clobber
python3 $D boottest /tmp/v3/xw.xex
```

`boottest` installs, sends `magicboot cold`, puts the fallback back on disk the moment xbdm
answers, watches the new build for 2 minutes and only then puts it back on disk. About five
minutes when the console comes back by itself; run it in the background and check `up` at
three minutes.

## Test (Mac, no console)

```bash
cc -O2 -Wall -Wextra -Itests/host -o /tmp/t tests/test_mictest.c && /tmp/t
cc -O2 -Wall -Itests/host -o /tmp/t tests/test_micdsp360.c && /tmp/t
cc -O2 -Wall -Itests/host -o /tmp/t tests/test_settings.c && /tmp/t
cc -O2 -Wall -Wextra -o /tmp/t tests/test_rumble.c && /tmp/t
cc -O2 -Wall -Itests/host -o /tmp/t tests/test_eq360.c -lm && /tmp/t tools/presets/xw.presets
cc -O2 -Wall -o /tmp/t tests/test_eq.c -lm && /tmp/t tools/presets/xw.presets dist/eq_vectors.bin
cc -O1 -Wall -w -Itests/host -DHP_HOST_TEST -DHUD_HOST_TEST -DXW_DEV -o /tmp/t tests/test_panel.c xbox360/hud_panel.c xbox360/panel_360.c && (cd /tmp && ./t /tmp/panel_out)
```

Host tests prove arithmetic and layout, not timing: only the console shows a stall.

## Gotchas

- **`magicboot cold` often switches the console off instead of rebooting.** It then answers
  nothing until someone presses its power button. The console never answers ping, on or off,
  and the Mac's ARP entry stays incomplete: only xbdm (`driver.py up`) says whether it is
  there. "No ping" once led to a wrong "frozen at boot" call while the owner was using it. Tell the owner within three
  minutes; do not wait silently. After a cold boot the controller is dark until its Xbox
  button is pressed, and xbdm can take a minute or more to answer.
- **xbdm's screenshot is not what the panel is drawn into.** At Aurora it returns an 864x480
  buffer without the panel. `driver.py panel` reads the HUD's own frame with getmem instead
  (`hud.py shot FILE --mem`), which takes about 8 s, so a moving background tears the picture.
  Read the text, not the pixels; `--find` reporting the four sentinels is the real check.
- **Letting go of Share inside 0.4 s toggles the mic mute** even if the panel had appeared.
  Injected holds must last at least 0.6 s.
- **A status block's four letters in a log string** can make the tools read the wrong address;
  `xa.py pe_va` searches `.data` first for that reason. Keep magics out of string literals.
- **A release build refuses dev-only commands** ("needs an XW_DEV build"): reload, trace,
  micdump, tone probes, `headset_test.py`.
- **Slow passes are per setting.** `python3 $D slow 6` before and after one change is the
  quickest way to pin a stall on a feature: on the build of 23 Nov 2026 any EQ preset with all
  five bands active added about 19 slow passes a second; FLAT and four-band presets added none.
- Never chain an install after an unrelated command with `&&`: a failed `cp` once skipped the
  install while the watcher still ran and swapped the fallback onto the disk.

## Troubleshooting

- `Cannot connect to the Docker daemon`: `open -ga Docker`, wait about 30 s.
- `git@github.com: Permission denied (publickey)`: `ssh-add --apple-load-keychain`.
- `cannot open dist/eq_vectors.bin`: `python3 tools/presets/convert.py --vectors`.
- `no XWST block in xw.xex on the console (an older build?)`: the build in memory is older
  than `xbox360/out/xw.exe`; the tools find blocks through the local file, so it must be the
  build that is running.
- `eq: no file worker, presets not loaded` or `CUSTOM: not offered`: the preset file on the
  console is missing or an older version; `python3 $D keep` copies the current one, then
  `(cd xbox360 && python3 eq.py reload)`.
- Console unreachable but the Mac's `en8` has no `inet 10.0.0.x`: replug the Mac's USB
  Ethernet adapter.
