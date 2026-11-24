# Wireless: notes for whoever picks it up

Wired pads only is the current scope. These notes record what reading the code showed about
adding wireless controllers (through the Microsoft adapter) to `xw.xex`, so the next person
does not start from zero. **Nothing here was tested.** Line numbers drift; names do not.

## Why the two plugins cannot run together

`xw.xex` (wired) and `xa.xex` (the original wireless plugin) both install the same three
XInput hooks from `xbox360/xinput_360.c` (`XInputdReadState`, `XamInputGetCapabilitiesEx`,
`XamInputSetState`), so they must never be loaded together. One plugin for both would need both
USB class-driver entries in one image, sharing `xinput_360.c`'s 4 pad slots (`g_xpad[4]`).

## What `xw.xex` already has, and what is missing

Already linked into `xw.xex`: `core/mt76.c`, `core/dongle.c` and `platform_360.c`
(`xbox360/build.sh`: `$XW_SRC $CORE`). Missing, all in `xbox360/xa_plugin.c`:

- the adapter's class-driver match (`XaMatchC`: 045e:02fe and 045e:02e6);
- the firmware loader (`load_firmware_at`, `load_all_firmware`; it reads from the hard disk
  or USB sticks, and calls `NtCreateFile` through `xa_kcall9`: see `docs/INTERNALS.md` for
  why);
- the dongle callbacks (`dongle_ops`: `client_added`, `client_removed`, `client_data`).

## Headset audio over the adapter

- Prior art: upstream pull request #7 on Jacckii/Xbox360-Series-Controllers-Native (open, by
  nobodys-coder) for the Xbox Wireless Headset 045e:0b32: game audio works, mic untested.
  (As summarised in `docs/ROADMAP.md` Part 2 B; not re-checked here.)
  - It uses the same kernel audio tap as `xbox360/game_360.c` (MEC client, ordinals 800 /
    802). The kernel has only two such slots, so one plugin must share one registration.
  - It hooks `XamVoice*` in XAM, where `voice_360.c` hooks `XVoiced*` (kernel ordinals
    481-483). The two approaches would need reconciling, not stacking.
  - It puts its driver thread on hardware thread 3 because "thread 1 is the title's main
    thread". Our HUD draw thread is on hardware thread 1 (`hud_360.c`, `thr_hw`).
- A headset in the jack of a *wireless* pad would need GIP audio on the radio queue
  (`XA_DONGLE_QUEUE_AUDIO`, `core/dongle.h`). The headset code in `core/gip.c` is
  transport-neutral: it calls the transport's `enable_audio` / `send_audio` ops
  (`core/gip.h`), which today only the wired USB path implements.

## Controllers

- SCUF Instinct Pro over wireless: untested. Share is assumed to be at the same report byte
  as wired (`xbox360/pad_quirks.h`); confirm it in a raw input packet received over the air
  before relying on it.

## Where the pointers are

Short comments point here from `xbox360/xinput_360.h`, `game_360.h`, `voice_360.h`,
`pad_quirks.h`, and next to `XW_SRC` in `xbox360/build.sh`.
