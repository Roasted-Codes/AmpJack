# Internals

How the driver works, and the Xbox 360 (kernel 17559) behaviour it depends on. Addresses are
for retail kernel 17559 and its XAM; ordinals are from the kernel's own export table.

## Layout

| Path | What |
|---|---|
| `core/` | Portable driver: MT76 chip (`mt76.c`), adapter and 802.11 (`dongle.c`), GIP protocol, security handshake and gamepad (`gip.c`), SHA-256/HMAC (`sha256.c`). Talks to the host only through `platform.h`. |
| `xbox360/` | The 360 plugin: Usbd backend (`platform_360.c`), class driver and driver thread (`xa_plugin.c`), XInput presentation (`xinput_360.c`), function hooks (`detour.c`), 64-bit register thunks (`thunks.s`). |
| `linux/` | The same core on libusb (`xadapter`), for testing on a PC or Raspberry Pi. |
| `third_party/micro-ecc/` | ECDH P-256 for the handshake. |

The core is a port of xone (`transport/mt76.c`, `transport/dongle.c`, `bus/`, `auth/`,
`driver/gamepad.c`). Linux infrastructure (URBs, skbs, workqueues, the kernel crypto API) is
replaced by a small platform layer: control and bulk transfers with completion callbacks, a
receive queue, a clock, allocation, logging.

## Adapter

One interface, class/subclass/protocol FF/FF/FF, USB 2.0 high speed: bulk IN 0x84 (commands)
and 0x85 (radio frames), bulk OUT 0x04–0x09, 512-byte packets.

Start-up: the firmware (≈70 KB) is uploaded with vendor control requests and bulk OUT; on a
warm adapter the running firmware is reset instead. Then the radio is initialised (calibration,
channels, MAC address, beacon) and pairing is switched on for 60 seconds. The adapter's pair
button (`XA_MT_EVT_BUTTON`) reopens pairing.

Controllers: 802.11 association, then GIP. The v2 security handshake uses ECDH P-256, a
transcript hash and an HMAC-SHA256 PRF to derive each controller's key; the key goes to the
adapter, which encrypts the radio link in hardware. Input reports, rumble and keep-alives are
GIP messages inside 802.11 QoS-data frames.

## USB on the 360

- **Claiming the adapter.** A class-driver entry linked into the kernel's runtime driver list
  (`0x801A8048`, walked by `UsbdSelectClassDriver`) matches 045e:02fe and 045e:02e6. No hooks are involved.
  The link is done in a DPC on Usbd's processor.
- **Transfer API** (hiddriver360 documents the same calls): 740 `UsbdAddDeviceComplete`,
  741 `UsbdCancelAsyncTransfer`, 746 `UsbdOpenDefaultEndpoint`, 747 `UsbdOpenEndpoint`,
  748 `UsbdQueueAsyncTransfer`, 749/750 close default/other endpoint,
  751 `UsbdRemoveDeviceComplete`, 758 `UsbdResetDevice`, 759 `UsbdGetDeviceDescriptor`.
  Queue, close and cancel return nothing; the outcome arrives through the transfer block's
  callback `(transfer, status)` on Usbd's DPC (a cancel completes with `0xC0000120`).
- **Usbd takes no locks.** `UsbdQueueAsyncTransfer` edits an unlocked qTD free list, and the
  kernel's own drivers only call Usbd from its DPC and timers. The driver thread therefore never
  calls Usbd: it posts requests to a ring that a DPC on Usbd's processor executes, and every
  transfer completion drains the ring too, so requests keep flowing when that processor is busy.
- **Outgoing traffic** is a FIFO with one transfer in flight, chained from each completion.
  The thread only appends and never waits, and protocol packets are never dropped.
- **Incoming traffic:** each IN completion copies its frame into a 128 KB ring and re-queues
  the transfer immediately, as xone resubmits its URBs; the driver thread drains the ring.
- **An IN buffer must fit one EHCI transfer descriptor** (≤ 0x5000 bytes from its page start).
  Usbd splits larger buffers without an alternate-next pointer, and after a short packet its
  bookkeeping and the controller disagree. IN buffers are page-aligned and sized accordingly.
- **A control transfer without data gets a NULL buffer.** Usbd adds a data stage whenever the
  pointer is set, even for zero bytes, and the device never completes such a request.
- Plain image memory works as a transfer buffer; Usbd translates each page itself.

## Picking up an adapter that's already plugged in

A device that no class driver claims is completed with an error by the kernel's add-device
path, and its hub port is marked in the unrecognized-port table (`0x801A7FA0`,
`{hub node, port bitmask}` × 20). `UsbdResetDevice` doesn't offer it to drivers again.

`UsbdTitleDriverResetAllUnrecognizedPorts` (ordinal 890) does: it re-queries unrecognized
devices and re-enumerates every marked port, at which point the linked class driver is asked
again. It takes a caller-owned context (≈0xF40 bytes, with a KEVENT at +0x1C that is set when
the work finishes), queues transfers and timers, and therefore runs on Usbd's DPC.

The plugin calls it once, after the title that was running when it loaded has been replaced
(the boot launching the dashboard, or the development loader handing over) and the new title
has been stable for 8 seconds: a title switch resets unrecognized ports itself, and the two must
not overlap. `UsbdGetDeviceDescriptor` can't identify a parked device (it returns the
descriptor of whichever device the controller enumerated last), so the rescan runs whenever the
table is non-empty; other parked devices simply stay unclaimed.

## Presenting controllers to the system

This follows hiddriver360's route:

- Each controller is bound as a virtual device with `XamUserBindDeviceCallback`
  (`0x816D9060`, not exported; its prologue is checked before use), which assigns a user index.
- Three functions are hooked: `XInputdReadState` (kernel 486) serves the controller state,
  `XamInputGetCapabilitiesEx` (xam 685) reports a standard gamepad, and `XamInputSetState`
  (xam 402) forwards rumble to the controller.
- The Guide button goes through XAM's registered Guide callback (the entry next to
  `XamUserBindDeviceCallback` in the kernel's table at `0x80189EA0`), the way the console's
  own wireless driver reports it. It fires on release after a short press.
- Every XAM call (bind, unbind, Guide, notifications) runs on a dedicated thread, never on the
  driver thread.

The hooks (`detour.c`) replace a function's first four instructions with a far branch and run
the originals from a trampoline. The branch is indirect, through a data word in the image
that patched the function, so a later build of the plugin takes a hook over with one store
instead of patching again (`docs/HOT-RELOAD.md`). Each trampoline builds its branch in a scratch register that
the relocated instructions don't use, decoded per opcode: the XAM functions start with
`mflr r12; bl __savegprlr`, and `__savegprlr` stores r12 as the return address. While a hook is
written, callers are parked on a `b .` instruction.

### Rumble and the Vibration setting

Read in the 17559 dumps, confirmed read-only on the console (`rumble_360.h`, `rumble_360.c`):

- `XamInputSetState` (xam 402, `0x816F1740`) maps the user to a device (`0x816D8A58`), then
  **ignores the title's flags** and passes the kernel's `XInputdSetState` (`0x800F6C88`) a scale
  from XAM's per-user byte table `0x81AAC2DC` (+0x3C..+0x44: `lis r11,0x81AB; addi
  r11,r11,-0x3D24; lbzx r4,r31,r11`): 3 if the byte is non-zero, else 0. The wired-pad driver
  (`0x800FA178`) sends `(speed / 3) x scale >> 8` per motor, so 0 = motors off.
- The table is the profile's **Vibration** setting, `0x10040003`
  (`XPROFILE_OPTION_CONTROLLER_VIBRATION`): XAM's work item `0x816F2618` reads it with
  `XamUserReadProfileSettings` for users 0..3 (3 for an index nobody is signed in on, the
  value's non-zero test otherwise), queued at sign-in and on notification 0x0E (profile
  setting changed, XAM dispatcher `0x816BDCFC`); boot sets all four to 3 (`0x816F4518`).
  Live on the owner's console: `00 03 03 03` (user 0 had switched Vibration off).
- When a title ends (XAM message 0x80000006, `0x81696328` -> `0x816F4660`), XAM calls
  `XamInputSetState(user, 0, {0, 0})` for users 0..3. Nothing in XAM or the kernel stops the
  motors when the Guide opens; the title does, through `XamInputSetState` like any request.
- Our hook replaces the kernel call for our pads, so it applies the byte itself (decoded from
  those three instructions at init, which our detour leaves alone). A GIP rumble command runs
  about ten minutes (duration 0xFF x 10 ms, 0xEB repeats) where a 360 pad only holds a level,
  so every stop is sent once more 100 ms later, and a title switch the plugin sees stops the
  motors too. Status block `XWRB` (`rumble.py`, `accept/rumble.py`).

Notifications use `XNotifyQueueUI` (xam 656). Its third parameter is a 64-bit value; it's
declared 32-bit so that the text and context land in r6 and r7, as XAM expects.

## Threads and priorities

The driver thread is a system thread pinned to hardware thread 2 (the affinity mask is the top
byte of `ExCreateThread`'s flags; without one, a system thread created from a DLL's entry point
is never scheduled). Hardware thread 2 also runs Usbd's DPCs and a XAM worker thread at
priority 6, which is busy while the guide is open. At the default priority of 5 the driver
thread would starve whenever the guide is open, so the thread raises itself to 9 with
`KeSetBasePriorityThread` (ordinal 153); 9 is the maximum for the system process. Its work is
short bursts between 2 ms sleeps, and at most 64 received frames are handled per pass.

### The system window: about 1 ms of CPU a frame

Read in the 17559 dump. On hardware threads 2-5 (`0x8017067C` = 0x3C) the kernel lets
priorities 0-9, the system's, run only in a short window after each vertical refresh:

- The display interrupt (`0x800D03B8`) calls `0x80074E58`, which flags a window start on each
  of those hardware threads (an IPI to the others).
- Their dispatcher (`0x800787D0`) loads the decrementer with `0x80170680` (50000 timebase
  ticks: **1 ms**) and sets the enabled-priority mask (PRCB +0x64) to 0xEDB403FF: 0-9 and some
  real-time levels. The ready-thread search (`0x80077518`) only picks priorities in the mask.
- The decrementer interrupt (`0x8007627C`) ends the window: the mask becomes 0xF6DBFC03,
  which has 0, 1 and 10-15 (titles) but not 2-9, until the next refresh.
- Enabled in both masks: 0, 1, 20, 23, 26, 29-31. `0x800D0468` can change the length (from a
  percentage of the frame); nothing of ours does.

So the driver thread (9, hardware thread 2) runs only in that window: one pass per frame,
16.6 ms at 60 Hz, whatever it sleeps (`xa_sleep_ms(1)` measured 14.9 ms), and **everything a
pass does must fit in about 1 ms**, Usbd's DPCs on the same hardware thread included. A pass
still running when the window closes stops for the rest of the frame (~15.6 ms) and finishes
in the next window; the draw-on-the-driver-thread pauses of `research/HUD.md` section 10 were
this. A 5-band EQ preset (about 300 us a pass) took the pass over it: half the passes took
33 ms. Hardware threads 0 and 1 have no window (the HUD's draw thread runs there at 15).

What the loop does about it (`xw_plugin.c`, XW36 version 3, `xw.py window`):

- A pass that the window stopped (its own time over 4 ms) is followed at once by the next
  pass, in the window it resumed in, instead of a sleep into the window after (at most once
  in a row). Counted as `win_cuts`.
- `win_boost` (xbdm, default off): before each earpiece block and between the polls, a pass
  past 5/8 of the window raises itself to priority 20 and finishes there; it goes back to 9
  after its sleep, where it waits for the next window anyway. The window's end then cannot
  stop it; it takes its overrun from the title's threads on hardware thread 2 instead.
- `run_us` / `run_max_us`: a pass's own time, loop top to sleep. That, not XWDG `work_us`
  (audio and voice only), is what has to stay under `win_us`. Budget new driver-thread work
  against what is left of the window, not against the 16.6 ms pass.

## Toolchain constraints

OpenXeChain's clang targets the 32-bit PowerPC SVR4 ABI, while the kernel is built for the
360's 64-bit-register ABI. Four rules follow:

- **Every function the kernel calls** (DllMain, DPCs, thread starts, transfer completions,
  driver callbacks, hooks) goes through a thunk (`thunks.s`) that saves r14–r31 as 64-bit values.
- **No kernel function that takes or returns a 64-bit value by value** is called directly: such
  a value would be passed in a register pair instead of one register.
- **No kernel function with more than eight arguments** is called directly: clang puts the
  ninth on the stack at `96(r1)`, the kernel reads it at `0x54(r1)`. `xa_kcall9` (`thunks.s`)
  takes the arguments as an array and places the ninth there. `NtCreateFile` called directly got stale stack data as its create options,
  and with `FILE_DELETE_ON_CLOSE` among them the firmware file was deleted once read.
- **No floating point.** DPCs have no FP context, and the ABI's variadic prologue saves f1–f8
  unconditionally, so a hard-float `printf`-style function faults in a DPC. The plugin builds
  with `-msoft-float`.

SynthXEX maps only the bytes the PE file contains, so zero-initialised data would fall outside
the loaded image; the plugin builds with `-fno-zero-initialized-in-bss`. `check_xex.py` enforces
both of the last two points.
