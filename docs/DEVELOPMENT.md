# Development

## Building

```bash
cd xbox360
OXC=/opt/openxechain/sysroot bash build.sh
```

`build.sh` builds:

| Output | What |
|---|---|
| `out/xa.xex` | The driver, a system DLL at `0x91500000` |
| `out/xa_loader.xex` | A development loader: a title that loads the plugin without a reboot, then launches Aurora |
| `out/xa_m3.xex` | A minimal probe that claims the adapter and reads two registers (bring-up aid) |

Options: `XA_HOOKS=read` installs only the input hook (no capabilities or rumble hooks), for
isolating problems. `XA_SLOT=n` builds `xa<n>.xex` at its own base address, so a new build can
be loaded next to a running one.

Never build with `-g`: the toolchain's clang crashes emitting DWARF into COFF.

### Release and development builds of `xw.xex`

`bash toolchain/build.sh` passes these through to the container (both default to 0):

| Switch | Effect |
|---|---|
| `XW_DEV=1` | Development build: compiles with `-DXW_DEV`. Probes, traces, dummy loads, dump buffers, alternative modes and verbose logs belong behind `#ifdef XW_DEV`; the release build (the default) leaves them out. Diagnostics are never deleted, only fenced, so future work can switch them back on. Console sessions that run the acceptance suite use `XW_DEV=1`. |
| `XW_TEAM=1` | Links the real console-to-console team voice (`team_360.c`, which includes `net_360.c`). The default links `team_360_stub.c`: team voice is not verified on two consoles, so it is not published. The source, `tests/test_team*.c` and `team.py` stay. |

For example `XW_DEV=1 XW_TEAM=1 bash toolchain/build.sh`. The build prints which team source
and flags it used (`xw.xex: team voice ..., release`).

What is in which build:

| Release (default) and `XW_DEV=1` | `XW_DEV=1` only |
|---|---|
| Every status block the tools and `accept/` scripts read (XW36 / XWAU, XWDG, XV36, XVHS, XWMT, XVGA, XH36, XWPN, XWHO), with the same layout | Hot reload, both sides (`reload_360.c`): release, take-over, adoption, the gates; slot builds (`XW_SLOT`) |
| XWPN `inject`, `xw.py tee` (it reads XW36, XWAU, XWHO, XH36, XV36 and the log) | The reload trace: `trace()` records nothing in a release build (the ring stays empty) |
| `pass_us` / `work_us` / `gap_us` and the pass histogram (XWDG) | The loop-timing probe (XWDG `probe`, `micdsp_360.c`) |
| The detour slots (`detour.c`): how every hook is installed | The audio self-test (audio stage 5's tone and the mic analysis), the 64 KB mic dump (allocated when `xw.py micdump` first asks), `usbreset`, `hsrestart`, `mecreset` (`selftest_360.c`) |
| The software sidetone, the test tone, the earpiece dump (`xw.py dump`) | `XW_STAGED` and `XW_NO_AUTH` (the build stops with an error without `XW_DEV`) |

A release build sets XWAU `release` (what was `reserved[0]`) to 1; `xw.py` then refuses the
`XW_DEV` commands (`reload`, `trace`, `gate`, `micdump`, `usbreset`, `mecreset`, `hsrestart`,
`audio 5`, `tone`, `diag probe`) with "needs an XW_DEV build" instead of waiting for an
answer. Fenced code is never deleted: the bodies are unchanged and compile again with the
switch. Release stubs (`xw_priv.h`, `micdsp_360.c`) do what the real code does when nothing
asks for it.

Install a build and reboot: `python3 xw.py install` (copies `out/xw.xex`, and `out/xw.presets`
if it is there, to `Hdd:\Plugins`, keeps the previous build as `xw.xex.prev`, reads the new
file back to check it, then `magicboot cold`; `--no-reboot` stops after the check).

### The plugin's files (`xw.xex`)

| File | What | Thread |
|---|---|---|
| `xw_plugin.c` | Class driver, USB transfers, headset audio (isochronous), the driver thread and its loop | Usbd's DPC / driver thread |
| `reload_360.c`, `selftest_360.c` | Hot reload; the audio self-test, mic dump and USB commands (`XW_DEV` only) | driver thread, some on Usbd's DPC |
| `xw_priv.h` | What those three share, and the release stubs | |
| `voice_360.c` | The virtual 360 headset (XVoiced hooks, packet rings), `xv_mic_samples` / `xv_earpiece_fill` / `xv_poll`: the mic chain and the earpiece mix, in the frozen order | hooks on title threads, the rest driver thread |
| `side_360.c` | Sidetone (software and the headset's own), headset volume, hiss control, centre tick, mic mute (XVHS) | driver thread |
| `micdsp_360.c` | The mic's DC removal, gate, meter, volume, limiter; the test tone; loop timing; `xd_poll` (XWDG). `mic_to_360` is the decimator's hook point | driver thread |
| `mictest_360.c` | The mic test and its contract adapters (XWMT) | driver thread |
| `voice_priv.h` | What the four audio files share | |
| `game_360.c` | Game audio: the render-thread tap and the stereo ring | render thread / driver thread |
| `panel_360.c`, `hud_360.c`, `hud_panel.c` | The Share panel: gestures and rows, the swap hook and draw thread, the drawing | driver thread / draw thread |
| `xinput_360.c`, `detour.c`, `platform_360.c` | XInput hooks and the XAM thread; the detour slots; Usbd calls, logging, allocation | |
| `eq_360.c`, `settings_360.c`, `file_360.c`, `surround_360.c` | Feature modules (stubs until their package lands); called from `xv_earpiece_fill` (`xq_process`) and the driver thread (`xf_init`, `xq_init`, `xs_init` at start; `xq_poll`, `xs_poll` every pass; `*_quiesce` / `*_resume` in hot reload) | |
| `kernel_360.h`, `dsp_360.h` | Shared helpers: `BARRIER`, `HOOK` / `BODY`, `struct sig`, the NV64 thunk macro; `clamp16`, `isin`, `isqrt`. Add to these instead of copying a helper | |

## Testing the core on Linux

`linux/` runs the same core on libusb, which is much easier to debug than the console. The
adapter must not be bound to xone:

```bash
sudo modprobe -r xone_gip_gamepad xone_dongle
make -C linux
sudo linux/xadapter /lib/firmware/xone_dongle_02fe.bin
```

It brings up the radio, pairs controllers and prints their input. `XA_DEBUG=1..3` adds frame
traces and raw dumps; `XA_NOSCAN=1` keeps the pairing channel fixed. `tools/readregs.c` reads a
few MT76 registers, for comparing what two hosts see.

`tests/test_sha256.c` checks the SHA-256/HMAC code against known answers:

```bash
cc -O2 -Wall -o test_sha256 tests/test_sha256.c core/sha256.c && ./test_sha256
```

## Working on the console

`xbox360/xa.py` talks to the console's debug monitor (xbdm, TCP 730). Set the console's
address with `XBOX=host[:port]`. Run it next to `out/`.

| Command | What |
|---|---|
| `xa.py upload [FIRMWARE]` | Copy the plugin, the loader and the firmware to `Usb0:\Apps\xadapter` |
| `xa.py boot on` / `off` | Add or remove the plugin as a `launch.ini` boot plugin (the old file is backed up first) |
| `xa.py load` | Start the development loader, which loads the plugin and launches Aurora |
| `xa.py status` | Driver state, log and controller states, once |
| `xa.py watch [SECONDS]` | Follow the log and controller states |
| `xa.py pair` | Open the pairing window |
| `xa.py unload` | Tear the driver down (hooks, endpoints, class driver) |
| `xa.py reload` | Experimental: build the next slot and hand the adapter over to it without a reboot |
| `xa.py reg ADDR [VALUE]` | Read or write an adapter register |
| `xa.py hold 0\|1`, `step` | Step through a USB removal one Usbd call at a time |

A loaded module's name stays taken until the console reboots, so after `unload` a new build of
the same name needs a reboot (or `XA_SLOT`).

The driver keeps a status block (magic `XA36`) and a log ring (magic `XLOG`) in its data
section; `xa.py` finds them through the build's PE file, so it must match the loaded build.

## Debugging tips

- **A stuck thread:** `getcontext` gives its registers; walk the stack's back chain from r1
  (SVR4 frames keep LR at +4 of the caller's frame, kernel frames at −8). Relinking with
  `-Wl,/map:` gives a map for the exact build (the output differs only in the PE timestamp).
- **Stopped or starved?** xbdm's `isstopped` tells a debugger stop from a thread that simply
  isn't scheduled. Briefly `suspend`ing other threads on the same hardware thread shows which
  one is taking the CPU.
- **Kernel ordinals** come from the 17559 kernel PE's export table (RVA + `0x80040000` is the
  live address), even though the PE's section table doesn't match the loaded image.
- **A hang right after loading** is usually a kernel callback without its thunk, floating-point
  code in a DPC, or data outside the image; `check_xex.py` catches the last two at build time.

## Code layout and conventions

- Kernel → plugin entry points are `XaXxx` thunks in `thunks.s`, with the C bodies named
  `XaXxxC` (`BODY`/`HOOK` attributes keep them from being inlined away).
- Usbd is only ever called on Usbd's DPC, through `xa_usbd_run` or the transfer helpers in
  `platform_360.c`.
- XAM is only ever called from the XAM thread in `xinput_360.c`.
- Kernel and XAM addresses that aren't exported are checked against known code before use.
