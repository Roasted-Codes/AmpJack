# Hot reload of xw.xex

Replacing the running wired-controller plugin with a new build, with no reboot, no replug and
no USB reset. The point: after a console reboot a wired Xbox Series pad stays dark until its
Xbox button is pressed, so every plugin update needed a person at the console. With a hot
reload the controller is never reset and stays switched on.

**Status (Nov 22 2026): the reload itself works in place; headset audio on a reloaded build
froze the console once (open).** Reloads through the loader title froze the console three
times, always around the loader title's exit (E1). In place (the old build loads the new one
from its own thread, no title switch) two reloads in a row passed (E1'), so in place is now
the default and the loader is `--loader`. On the second reloaded build a headset replug
opened audio, and the console hard-froze about 5 s later
([Open: audio on a reloaded build](#open-audio-on-a-reloaded-build)). The builds carry a
trace and staged gates ([Trace and gates](#trace-and-gates-wp31)); see
[Experiments](#experiments-wp32) and [Unverified](#unverified).

WP3.3/3.4 (after E1'): the headset is restarted after a take-over with the format it had,
not asked to identify itself (it ignores that; [Headset after a reload](#headset-after-a-reload));
the user's settings and the mic mute carry over; `xw.py reload` stops if another module
patched one of our functions.

```bash
cd xbox360
export XBOX=10.0.0.13 XW_DIR='Hdd:\Plugins'   # the console, and where the plugin lives on it
python3 xw.py reload            # build the next slot, release, load, take over
python3 xw.py handover          # what every loaded build says about itself
```

The build that is running must already support it. A build from before this feature has no
handover block; `xw.py reload` says so and changes nothing. Install a build from this branch
as the boot plugin and reboot once (the last time a button press is needed); every update
after that can be a reload.

## How it works

A loaded module's name and address can't be reused until the console reboots, and code that
another thread may be executing can't be freed. So nothing is ever unloaded:

1. **The new build is a separate module.** `XW_SLOT=n` builds the same source as `xw<n>.xex`
   at its own base (`0x92800000 + (n-1) MB`). `xw.py reload` picks the next free slot.
2. **The old build releases.** It quiesces (below), publishes a `struct xw_handover`
   (`xbox360/handover.h`, magic `XWHO`) and ends its driver thread. Its image stays loaded
   as dead weight plus a few passive entry points.
3. **The new build takes over.** A slot build does nothing until `xw.py` gives it the address
   of the old build's handover block. It checks the block, inherits the hooks, links its
   class driver, and adopts the controllers.

Three mechanisms make the hand-over safe.

### Hooks: one store, never a patch

A detour used to overwrite a function's first four instructions with a jump straight to the
hook. Taking it over would mean rewriting those four words while other threads call the
function, and a thread that has executed two of the old words and then two of the new ones
jumps to garbage. Unpatching has the same problem.

Now the patch is an indirect jump through a *slot*, a data word in the image that installed it
(`detour.c`):

```
lis r12, slot@ha ; lwz r12, slot@l(r12) ; mtctr r12 ; bctr
```

The code is patched once per function and console boot. A later build takes the hook over by
storing its own hook's address in the slot: one aligned 32-bit store, which a caller sees
either before or after. Every hook a build knows (its own and inherited ones) is listed in its
`detour_registry`; the next build copies the list (`detour_inherit`) and `detour_install`
adopts any function in it instead of patching.

A thread that is inside an old hook, or inside an old trampoline, simply finishes there: the
old image is still loaded. The original code keeps being run through the trampoline of the
build that first patched the function, however many builds later.

Between the release and the take-over the old hooks stay in place:

- the XInput hooks pass everything through (no pad is bound any more);
- the three XVoiced hooks keep answering "no headset" for our device tags, as they do below
  voice stage 3. This shield must never drop: the kernel's own code would index past its
  wireless table with our tags (`voice_360.c`, top).

### Controllers: adopted without a USB reset

For each usable controller the old build publishes Usbd's device record (`node`), the handles
of its open interrupt IN/OUT and default endpoints, and the audio endpoint addresses. The new
build copies the handles into its own transfer blocks and writes its own class driver into
the device record (the word at `node + 4`), so the device's removal is reported to the new
build. Nothing is sent to the device, and nothing is opened or closed.

Checked in the 17559 kernel dump: `UsbdOpenEndpoint` (OHCI: `0x800DC310`) writes only the
block's first word, the endpoint object, and keeps no pointer to the block; the add path
stores the selected driver at `node + 4` (`0x800D6F98`) and removal calls
`((driver *)node[1])->remove_device(node)` (`0x800D6748..0x800D676C`, and `0x800D66B4` for
interface nodes), reading the pointer at that moment.

The controller is still switched on and still configured. It won't announce itself again, so
the new build asks it to identify itself at once (`gip_adopt`), and goes on as after an
announce: power on, LED, security handshake, input. If the old build had a headset, the new
one asks for it 1.5 s after the pad is ready (`gip_headset_probe`), unless it has announced
itself by then; the headset then goes through the normal start (SET_INTERFACE, isochronous
endpoints, format, stream).

### The one transfer that can't be finished: forwarded

One interrupt IN transfer is always queued on a controller, and an idle controller sends
nothing, so that transfer can stay queued for hours. It is never cancelled. It stays where
it is, inside the old image, and the new build queues none of its own while it is there (two
IN transfers on a full-speed interrupt endpoint froze the console). When it completes, the
old image's completion clears `in_busy` and calls the adopter's `in_fwd` on Usbd's DPC; the
adopter takes the packet and queues its own transfer. In practice that is the controller's
answer to the identify request, a few milliseconds after the take-over.

If the build that adopted the controller is itself replaced before that happens, the next
build is pointed at the same entry (`in_entry`), in whichever older image it lives.

## What happens to each thing the old build holds

| Thing | At the release | After the take-over |
|---|---|---|
| Class-driver registration | Unlinked from Usbd's driver list, on Usbd's DPC | The new build's driver is in the list |
| Claimed controllers (nodes) | Left claimed and configured; published | Adopted: handles copied, `node + 4` swapped |
| Interrupt IN transfer | Left queued, never re-queued by the old build | Completes into the old image, forwarded |
| Interrupt OUT queue | Drained (waited for) | New build's own queue |
| Default endpoint | Idle (no control transfer in flight, or the release is refused) | Handle copied |
| Isochronous audio blocks | Not re-queued; waited for until all have completed; never cancelled | New blocks |
| Audio endpoints | Closed, interface 1 back to alternate 0 | Reopened when the headset identifies |
| Pads in XAM | Unbound (headset first, then pad), on the XAM thread | Bound again by the new build |
| XAM thread | Drained, then it exits | The new build's own |
| XInput hooks (4) | Stay patched; pass-through | Slots point at the new build |
| XVoiced hooks (3) | Stay patched; shield ("no headset" for our tags) | Slots point at the new build |
| Virtual headsets | Unbound, queued packets failed | Bound again when the headset is back |
| MEC game-audio client | Tap off, then `XAudioUnregisterRenderDriverMECClient` (it waits for a running callback) | The new build registers its own after 20 s |
| VdSwap hook (`hud_360.c`, later) | `hud_quiesce`: draw nothing, pass through | Adopted like the others |
| Driver thread | `ExTerminateThread` after the release | The new build's own |
| Log ring, status blocks | Stay readable in the old image | The new build has its own; `xw.py` follows it |
| gip clients, dump buffers (heap) | Left allocated (a few KB; up to ~860 KB if `xw.py dump` was used) | |

A controller whose endpoints never opened (claimed but unusable) is not published. It stays
with the old build, whose removal path still works, until it is unplugged.

To a title the reload looks like the controller being unplugged and plugged back in: the pad
is unbound and bound again and normally gets the same player slot. Headset audio is back a
few seconds later.

### Memory cost

Each reload leaves one more image loaded: 692 KB today (`0xAD000`), plus the heap noted
above. Ten reloads cost about 7 MB of the console's 512 MB. A reboot frees all of it and puts
`xw.xex` (slot 0) back. Slots go up to 24.

## The release, step by step

`do_release` in `xw_plugin.c`, on the old build's driver thread. Everything that can fail
comes first and can be undone; the hand-over itself is one routine on Usbd's DPC.

1. Preview check: no controller being removed or still being set up, audio not broken, and
   Usbd's record of every controller names this build's driver. Otherwise refuse; nothing
   was touched.
2. Audio blocks: streaming is switched off (nothing is re-queued or submitted), then every
   queued isochronous block is waited for (300 ms; 40 ms of blocks are in flight). If they
   don't complete, the streams are started again and the release is refused.
3. Headset off (`aud_disable`): the 360 is told the headset is gone, the audio endpoints are
   closed (nothing is queued on them now), SET_INTERFACE alternate 0.
4. Game audio: tap off, MEC client unregistered. Then every pad is unbound in XAM, the
   virtual headsets are quiesced, `hud_quiesce`.
5. The XAM thread is waited for until it has carried out every unbind and is idle (2 s).
6. The controllers' OUT queues are waited for (300 ms).
7. **Commit**, on Usbd's DPC (`commit_dpc`): every condition is checked again there, where no
   completion, AddDevice or RemoveDevice can run in between. If all hold: the class driver is
   unlinked, each controller is published and marked handed. Otherwise nothing changes.
8. Final: the XAM thread exits, state `released`, the driver thread ends.

Steps 2 to 7 can refuse. After a refusal past step 2, `release_resume` puts things back: the
voice and game-audio sides resume, the pads are bound again, the headset is asked to identify
itself again. The old build keeps running.

| Refusal | Meaning |
|---|---|
| headset audio is broken | An audio USB request never completed earlier, so blocks may still be on Usbd. Only a reboot is safe. |
| audio endpoints didn't close / blocks still queued | Isochronous blocks didn't drain, or the close didn't finish |
| a controller is being removed / set up | Try again in a moment |
| the XAM thread is still inside XAM | The unbinds aren't done (a system dialog?). Try again |
| outgoing packets didn't drain | An OUT transfer is stuck |
| the hand-over routine never ran | Usbd's DPC didn't run it within 1 s |
| Usbd's record doesn't name this build's driver | The kernel isn't laid out as read; nothing is handed over |

Each refusal is one log line starting `reload: REFUSED:`; `xw.py reload` prints it with the
log's tail and stops.

## If something goes wrong after the release

Once the old build has released, it can't be restarted (its thread is gone), but nothing is
lost: the controllers stay claimed and powered, and the handover block stays valid.

| Situation | What to do |
|---|---|
| The new module didn't load (`xw<n>.xex did not load`) | Fix it and run `xw.py reload` again: it sees the old build is released and carries on from there |
| The new build refused to take over (wrong address, version, not released) | It stays idle and hooks nothing. Run `xw.py reload` again: the next slot takes over from the released build |
| `xw.py` was interrupted before the take-over | The slot build waits 60 s, then goes idle. Run `xw.py reload` again |
| A controller wasn't adopted (`left alone`) | It stays with the old build. Replug it, or reboot |
| The pad is silent after the take-over | `xw.py usbreset 0` (re-enumerates it into the running build), or replug |
| Anything else | Reboot: everything is back to `xw.xex` |

`xw.py reload --reset` doesn't adopt: the new build resets each controller (as `xw.py
usbreset`), Usbd has the old build close it, and it re-enumerates into the new driver. That is
the path `xa.xex` uses. It is a fallback only: the repo's notes say a USB reset re-enumerates
a pad cleanly and doesn't wake a dark pad, but nothing records whether a lit pad stays lit
through one.

`xw.py reload --in-place` has the old build's driver thread load the new module itself
(`XexLoadImage` from a system thread, the process-type byte set as `loader.c` does), instead
of the loader title. No title switch happens, so a game keeps running. Without it, the loader
title runs and then launches Aurora, as `xw.py load` does.

## Review of the teardown paths

Who can still be running in the old image, and why each case is safe.

| Path | Race considered | Why it holds |
|---|---|---|
| Hook entered while the slot is switched | A caller reads the slot before or after the store | One aligned word; both targets are loaded for good. `struct detour` (the trampoline pointer the new hook uses) is written before the slot, with a barrier |
| Thread inside an old hook body or trampoline | The image being replaced under it | The image is never unloaded and its code never rewritten |
| Old XInput hooks after the release | They read pad state the thread no longer updates | `xi_quiesced` and `active = 0`: pure pass-through |
| Old XVoiced hooks after the release | Packets queued for a headset nobody serves | Unbound first (`serving = 0`, queued packets failed under the lock); later submits answer "no headset". The lock, rings and event log stay valid |
| MEC callback on the render thread | Unregistered while it runs | The kernel's unregister yields until the render thread has left this client's callback. If the kernel's code isn't the one read, the client stays registered and returns at its first test (`g_tap_on = 0`), costing one of two MEC slots |
| XAM thread | Stopped while inside an XAM call | It is drained first (queue empty and not inside a request) or the release is refused; it stops only after the commit |
| Open menu box (diagnostics) | XAM writes the result later | It writes into the old image's static storage, which stays; nobody reads it |
| IN completion vs. the commit | Re-queue after the hand-over | Both run on Usbd's DPC. After `handed` the completion never re-queues |
| IN completion vs. adoption | The new build queues while the old transfer is still on the endpoint | `in_busy` is read and `in_fwd` written by the adoption on Usbd's DPC, where the completion can't run in between |
| OUT | A packet submitted after the hand-over | The thread is in `do_release`, not its loop, so nothing new is queued; the commit refuses unless every queue is empty; `tx_submit` returns once `handed` |
| Control endpoint | A control transfer in flight | `setup_step` must be 0 at the commit; the audio alternate-0 request is waited for and marks audio broken on a timeout, which refuses |
| Isochronous blocks | Closed or freed with blocks on Usbd | Waited for before the close; the commit refuses if any is queued, busy or staged, or the endpoints are open or closing |
| Commit vs. unplug | The device goes while it is handed over | `removing`/`gone` are checked on the DPC. Unplugged later and not adopted: the old build's RemoveDevice still closes it and takes it off offer. Adopted: removal goes to the new build |
| Commit DPC that runs late | The thread gave up and resumed, then the DPC hands over anyway | `dpc_once`: the thread's give-up and the routine's start are one compare-and-swap; only one of them wins |
| Adoption vs. a stale offer | The record was freed or re-used | `node + 4` must still name the old build's driver, checked on the DPC, before anything is written |
| New device during the gap | Offered to nobody | Between the unlink and the new link, a newly plugged controller is unrecognized; replug it |
| Two builds on one controller | Both think they own it | `e->node` is cleared by the adoption; the old build queues nothing once `handed` and has no thread |

Known gaps, accepted (an independent review of the code found no crash path; these are what
it left open):

- A controller that enumerates while no xw driver is linked (from the commit until the new
  build links its own; longer if the load fails) lands in Usbd's unrecognized list. The new
  build doesn't rescan after a take-over, because a rescan that overlaps the loader's title
  switch can hang the console. Replug it.
- A controller the new build didn't adopt stays in the old build's block. A later reload
  only reads its direct predecessor's block, so it is not picked up then either. `xw.py
  reload` prints a warning; replug it.
- A packet the controller sends between the release and the adoption is dropped (one IN
  transfer is queued and nobody is listening). Input reports carry the whole state, so the
  next one corrects it; a Guide press in that window is lost.
- A refused release binds the pads again in port order, so with several pads the player
  numbers can change. Rumble isn't switched off at a release: a motor that was running keeps
  running until the title next sets it.

Not covered, by design: the very first patch of a function (at boot, or a hook a later build
adds) still rewrites four instructions with callers parked on a `b .`, as before. Reloads add
no such moment for hooks that already exist.

## For `hud_360.c` (the VdSwap hook)

`hud_360.h` now has `hud_quiesce()` and `hud_resume()`; the stub implements them as no-ops.
The real overlay must:

1. install its VdSwap hook with `detour_install`, so it is in the registry and the next build
   adopts it with one store (there are 12 trampolines; 7 are used);
2. in `hud_quiesce`, make the hook a pure pass-through from its return on: draw nothing, read
   nothing the driver thread owned. It must not unpatch;
3. call the original only through its `struct detour`'s trampoline, so an adopted hook keeps
   working;
4. keep per-frame state in its own image: after a reload the old image's copy is dead.

## Compatibility between builds

`struct xw_handover`, `struct xw_ho_port` and `struct detour_registry` are read by a build
other than the one that wrote them. Append fields only. If a field's meaning changes, bump
`XW_HO_VERSION`: a build only takes over from its own version, and `xw.py` refuses to try.
The `in_fwd` call (`void fn(void *node, const u8 *data, u32 len, long status)`) is part of it.

## Trace and gates (WP3.1)

`docs/PLAN.md` WP3.1. The aim: when the console freezes, the log that survives (`xw.py tee`)
names the step that was running.

**Trace.** Every reload step writes a code (`TR_*` in `xw_plugin.c`, mirrored by
`TRACE_NAMES` in `xw.py`) to the handover block's `trace` word, and the code and the
timebase into a 64-entry ring (`struct xw_trace`, magic `XWTR`, address in `trace_ring`;
`trace_n` counts the codes written). One atomic add and three stores, so it is safe on
Usbd's DPC. The steps traced: each step of `do_release` and `commit_dpc`; in the new build
`takeover_import`, the XInput hooks, the class-driver link and its DPC, `adopt_dpc` (each
port, skip or reset), the take-over's end and its "ready" notification; on an adopted
controller the forwarded IN completion (`XwInForwardC`) and the transfer it queues, the
first IN and the first OUT queued, the gip client, `gip_adopt`, the pad's XAM bind; and in
any build `XwRemoveDeviceC` and the headset's isochronous open (`aud_open_dpc`). Not traced:
XVoiced's `install_hooks` and the MEC `try_register` (`voice_360.c` and `game_360.c` belong
to another package); gates T8 and T10 put a step boundary around them instead, and the tee
watches the XV36 block.

`xw.py tee` reads the trace ring of every loaded build every 50 ms and writes each new code
as a line (`xw1.xex trace 17: 00000281 ADOPT_PORT port 0 (+0.4 ms)`), along with changes of
`cmd_gate`, `gate_at`, `title` and `in_double`. `xw.py trace [NAME]` prints the rings;
`xw.py handover` prints the last step.

**Gates.** The handover block's `cmd_gate` (99 at load: nothing holds). A step runs only once
`cmd_gate` has reached its gate's number; while a build holds, `gate_at` is `'R'` or `'T'`
<< 8 | n, the log says `reload: gate T3 (...): holding`, and the trace has `GATE_HOLD T3`. A
gate opens by itself after 300 s, so an interrupted `--step` can't leave a build held for
good; `xw.py gate 99` opens them all at once.

| Gate | Build | Holds before |
|---|---|---|
| R1 | old | the audio blocks are drained |
| R2 | old | headset off, audio endpoints closed, interface 1 to alternate 0 |
| R3 | old | game audio off, MEC client unregistered |
| R4 | old | pads and headsets unbound in XAM, XAM thread drained |
| R5 | old | the commit on Usbd's DPC |
| R6 | old | the XAM thread and the driver thread end |
| T1 | new | the previous build's block is checked and its hooks inherited |
| T2 | new | XInput: XAM thread started, the 4 slots stored |
| T3 | new | the class driver is linked |
| T4 | new | `adopt_dpc` |
| T5 | new | the adopted controller is served: gip client, IN started (until then `XwInForwardC` doesn't queue our IN) |
| T6 | new | `gip_adopt` (the identify request) |
| T7 | new | the pad's bind in XAM |
| T8 | xw.py | voice stage raised (XVoiced slots stored) |
| T9 | xw.py | audio stage raised and `hsrestart`: SET_INTERFACE, iso open, stream |
| T10 | xw.py | game audio enabled: MEC registered |
| T11 | xw.py | HUD enabled: VdSwap slot stored |

R gates hold only during a release, T gates only in a build that is taking over (a refused
release's `release_resume` binds the pads again without holding). T8 to T11 are the new
build's own settings: with `--step`, `xw.py` writes 0 to all four before the take-over and
raises each to the old build's value at its gate (a gate whose value is 0 is skipped). A
build from before WP3.1 has no gates: `--step` then releases it in one go and steps through
the new build only.

**`xw.py reload` options.**

- `--step`: one gate at a time. At each it prints the new trace codes and log lines, says what
  the next step does, and waits for Enter. `--hold [S]` holds S seconds instead (default 15;
  also used when stdin isn't a terminal).
- `--settle`: `cmd_takeover` is written only once the title has been the same for 10 s (the
  driver thread publishes `XexExecutableModuleHandle`'s value in `title`). With the
  loader title it also waits for the switch to Aurora (30 s at most), and gives up after 40 s
  (the slot build waits 60 s for its take-over word).
- `--no-auth`: `cmd_mode` 2: adopt, but skip the second security handshake (a diagnostic for
  cause 6; a Microsoft pad then shows no headset).
- Every reload now gives the new build the old one's voice stage, audio stage, game audio
  enable and HUD enable before it takes over (it used to start with its defaults).

**Guards.** `queue_in` refuses, logs and counts (`in_double`) an interrupt IN transfer when
one is already queued on the endpoint (ours, or an earlier build's: `in_wait`). It must stay
0. `XwInDoneC` now clears the block's `busy` before it queues it again. `adopt_dpc` logs the
handles it copied for each port.

The words are in what was `reserved[0..6]` of `struct xw_handover`: size and version are
unchanged, so a build with them takes over from one without them and the other way round.

## Headset after a reload

A running jack headset ignores an identify request: `gip_headset_probe` after a take-over and
`xw.py hsrestart` (`gip_headset_reidentify`) never brought it back; only a replug did. Now:

- `gip_headset_resume(gc, in, out)` (`core/gip.c`) restarts it with a format it accepted
  before, with no identify: audio on (SET_INTERFACE 1/1, iso open), the headset to sleep,
  the format offered, power on, as after an identify. If it doesn't answer the format within
  1 s the format is taken as accepted; an audio packet meanwhile counts as the answer. Then
  the usual power-on retries (6 x 500 ms) and "headset ready".
- The client remembers the last accepted format (`hs_known_in/out`, kept across a headset
  loss), so `xw.py hsrestart PORT` uses it; `xw.py hsrestart PORT IN OUT` gives one (hex:
  `0 9 10` = 24 kHz mono mic, 48 kHz stereo earpiece, what 045e:02f6 negotiates).
- The release hands it over: `xw_ho_port.hs_fmt` (in | out << 8 | `HO_HS_FMT_VALID`, in what
  was `reserved[0]`). 1.5 s after the adopted pad is ready the new build resumes the headset
  with it; without one (an older build) it still probes.
- At audio stage 0 the resume is refused (as before), but the format is kept: `xw.py audio 4`
  + `xw.py hsrestart 0` later brings it back.

## Settings across a reload (WP3.4)

`xw.py reload` reads the old build's settings before the release and writes them into the
new build before its take-over (all xbdm words the user's controls change): voice stage,
audio stage, game audio enable, HUD enable (gates T8-T11 with `--step`), and balance, master,
sidetone (XVGA), mic gain, noise gate, game buffer (XWDG), team stage, channel, level
(XWTV). The mic mute has no xbdm word: the release marks the port `HO_PF_MUTED` and the new
build mutes that pad after its bind. Acceptance: `accept/p3_settings.py` (balance -3 and gate
high survive a reload; `--hands` also the mute).

## Open: audio on a reloaded build

Nov 22 2026, 20:06, log `out/logs/20261122-195639.log`. xw.xex -> xw1 -> xw2, both in place.
xw2: game audio registered at 20:03:05 (the third MEC registration this boot: xw.xex and xw1
each registered and unregistered handle 801960ac), HUD at stage 3 with VdSwap freshly hooked
(nothing had hooked it before; draws stayed at 70 to the end). the owner replugged the headset:
SET_INTERFACE 1/1, iso open, headset ready, mic 1002/s, the game tap copying for the first
time since the first unregister. 5 s later, during D-pad presses, a hard freeze (xbdm dead).
The iso endpoints had last been closed at 19:59:18 by xw.xex (audio 4 -> 0, before E1').

Read and checked against the dump, nothing old the kernel can still call was found:

- MEC: unregister (0x8012DBA0) waits while the render thread is in that client's callback,
  then zeroes the slot's callback word; register (0x8012D828) copies the callback and argument
  into a free slot (callback 0); the render loop calls the slot's copy. No pointer into the
  image that registered stays with the kernel.
- The old builds' iso blocks: all completed before their closes (xw.xex: ear 5608 = out_done
  5608); their completions only count a block of another open as stray. Nothing of theirs is
  queued.
- The old builds' interrupt IN: completed (`IN_HANDED` in xw1 at 20:02:56), not re-queued.
- Hooks: every slot points at xw2; the old bodies are not called.

So the two events that happened for the first time are the suspects: (a) an iso open after
an earlier close (in another image, but the same Usbd endpoint and TD pool), and (c) the
MEC tap copying after an unregister and a new registration. Neither needs a reload to happen,
so the repro below first tries each on a cold build. New trace codes for it: `AUD_CLOSE`,
`AUD_CLOSED` (with status), `AUD_FORCE`, `AUD_ALT`, `ISO_IN_FIRST` and `ISO_OUT_FIRST` (the
first completion after an open, with its first frame's status), `ISO_STRAY` (a completion for
a block of another open: 1-4 = which check), `MEC_RESET`, `MEC_RESUMED`. `xw.py mecreset` =
unregister and register the MEC client again in the running build.

## Experiments (WP3.2)

The coordinator runs these at the console, in this order, cheapest discriminator first. Each
successful reload is the next one's baseline, with no reboot. A freeze costs a reboot and
the owner pressing the pad's Xbox button.

**Every session.**

```bash
cd /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360
export XBOX=10.0.0.13 XW_DIR='Hdd:\Plugins'
python3 xw.py tee            # in its own terminal, the whole session; log in out/logs/
python3 xw.py hooks          # expect: 7 ours, 1 original, 0 foreign (exit 0)
python3 xw.py handover       # which build runs, and how many are loaded
```

Console idle at Aurora, pad on, headset in the jack. **If it freezes:** the tee prints
`CONSOLE SILENT since <t>`. The step that froze is the one after the last trace line before
that time (after a `GATE_OPEN Tn`, it is step Tn). Note whether the tee prints `CONSOLE BACK`
within a minute without a reboot (xbdm survived: a thread spinning, not a DPC or the kernel).
Then power-cycle, press the pad's button, and send the tee log to engineer F.

**E1, minimal path** (no audio, no MEC registration in the new build, no title-switch overlap).
The first reload starts from the installed WP0.1 build, which has no trace and no gates: its
release runs in one go, as before. Note: `voice.py game off` doesn't unregister a MEC client
that is registered already, so this release still unregisters the boot build's client
(cause 3, release half); its log line (`game: quiesced, MEC client ... unregistered`) reaches
the tee before anything else happens.

```bash
python3 xw.py audio 0 && python3 xw.py hsrestart 0   # headset refused: status shows no streaming
python3 voice.py game off                            # stays registered, but the new build never registers
python3 hud.py status                                # disabled (enable = 0), not hooked
python3 xw.py reload --step --settle
```

Expected, in order: `replacing xw.xex (running) with xw1.xex`; `xw.xex: voice stage 4, audio
stage 0, game audio enable 0, HUD enable 0`; `xw.xex has no gates (built before WP3.1)`;
`xw.xex: released`; `load 200- ...` (screen black, Aurora back); `new build starts with: voice
stage 0 (then 4), audio stage 0, game audio enable 0, HUD enable 0`; `--settle` printing
two titles and `settled` about 11-13 s after the load; `xw1: take over from ...`. Then a
`--- xw1 Tn next:` prompt for T1 to T7, each after these trace codes:

| Prompt | Trace codes just before it |
|---|---|
| T1 | `TAKE_WAIT`, `TAKE_CMD`, `GATE_HOLD T1` |
| T2 | `TAKE_IMPORTED`, `GATE_HOLD T2` |
| T3 | `TAKE_HOOKS`, `TAKE_HOOKED`, `GATE_HOLD T3` |
| T4 | `TAKE_LINK`, `LINK_DPC`, `TAKE_LINKED`, `GATE_HOLD T4` |
| T5 | `TAKE_ADOPT`, `ADOPT_BEGIN`, `ADOPT_PORT port 0`, `ADOPT_DONE`, `TAKE_ADOPTED`, `TAKE_NOTIFY`, `TAKE_RUNNING`, `GATE_HOLD T5`; log `port 0: handles copied: IN ..., OUT ..., default ...; IN transfer of entry ... still queued` |
| T6 | `PORT_GC port 0`, `GATE_HOLD T6` |
| T7 | `FIRST_OUT port 0` and `PORT_ADOPT_SENT port 0` (either order), `IN_FWD port 0`, `FIRST_IN port 0`, `IN_FWD_QUEUED port 0` (the identify answer through the old build's transfer), then the handshake's traffic (not traced), `GATE_HOLD T7`; log `pad 0: ready` just before |

then `PAD_BIND pad 0`, `PAD_BOUND pad 0`; `T8 next: voice stage back to 4`; T9, T10 and T11
`stays 0`; `take-over done: 1 adopted, 0 reset, 0 left alone, 7 hooks inherited`; `trace:
last step ...; gate 99; in_double 0`; `done: xw1.xex is the running build`. Afterwards
`xw.py hooks` says 7 ours (now jumping to xw1). the owner check: the pad's light stays on
throughout, and Aurora reacts to the pad afterwards.

Reading: E1 passing makes causes 5-8 unlikely and shows the mechanism works. If E1 freezes,
the step is named; then reboot, and repeat E1 with `--reset` (no copied handles: cause 7),
then with `--no-auth` (no second handshake: cause 6).

**E1 result (Nov 22 2026, build 5125e1a, `--hold 10 --settle`): froze before any take-over.**
R1-R6 held and opened cleanly; the release ended `REL_DONE` with port 0 on offer and its IN
transfer still queued. `load 200- OK`; the loader title loaded xw1, which started and waited
for its take-over word (`--settle` was still running). The console went silent about 0.5 s
after xw1's first log lines and never came back: within the loader title's 1 s before it
launches Aurora, or at that launch. So the freeze needs none of the take-over: it is in the
window where the old build is released, xw1 is loaded and idle, and the loader title exits.
The earlier "froze right after take over" fits the same moment. Three suspects remain, none
of them visible in the code (the paths were read: a released build's IN completion only
clears `in_busy` and forwards to nobody; the quiesced XInput, XVoiced and VdSwap hooks pass
through; xw1 before its take-over word only sleeps):

- (a) the released state itself (the IN completing into the released build, or anything
  else that runs while nobody serves the pad);
- (b) a title switch while the class driver of a claimed device is unlinked from Usbd's list
  (between the commit and the next build's link; Aurora's exit to the loader passed this
  way once already);
- (c) the loader title exiting after it loaded a slot build (its module and thread).

**E1', in place** (no title switch at all: tests (c) against (a)+(b) at once, and is the
path to keep if it works). After the reboot, set up as for E1, then:

```bash
python3 xw.py reload --in-place --hold 10 --settle
```

Expected: R1-R6 as in E1; `xw.xex: released`; the old build's trace `REL_LOAD_NEXT`, its log
`reload: loading \Device\Harddisk0\Partition1\Plugins\xw1.xex from this thread`, `reload:
XexLoadImage: 00000000`, trace `REL_LOADED`; `xw.xex: XexLoadImage status 00000000`; no
`load` line and no black screen; `--settle` prints one title and `settled` after 10 s; then
T1-T7, T8 and `done: xw1.xex`, as in E1. A freeze between `REL_LOAD_NEXT` and `REL_LOADED`
is `XexLoadImage` from the driver thread (Unverified 6).

If E1' passes, (c) or the title switch is the trigger and `--in-place` becomes the default
(WP3.3). To learn which, with a reboot each time something freezes:

1. **E1''a, the released state alone:** `python3 xw.py reload --release-only --hold 5`. Stay at
   Aurora for 60 s; the owner presses A a few times (no input reaches Aurora now: expected). The
   tee shows `IN_HANDED port 0` in the released build's trace at the first press: the IN
   completed into the released build and was not re-queued. A freeze here is (a).
2. **E1''b, a title switch while released:** `python3 xw.py aurora` (relaunches Aurora from
   `Hdd:\Aurora`). Wait 30 s. A freeze is (b).
3. **E1''c, the loader:** `python3 xw.py reload --no-build --hold 5 --settle` (goes on from the
   released build: the loader title loads the slot built in step 1 and launches Aurora). A freeze here, with
   E1''a and b clean, is (c).

**E2, the headset on the running slot build** (cause 2 alone, no reload).

```bash
python3 xw.py audio 4 && python3 xw.py hsrestart 0
python3 xw.py status         # within ~5 s: IN open, OUT open, streaming; mic ~1000/s
python3 xw.py trace xw1      # AUD_OPEN port 0, AUD_OPENED port 0
python3 voice.py status      # headset bound again
```

A freeze here, with `AUD_OPEN` the last code, is the open itself; with `AUD_OPENED` last, the
first blocks queued after another image closed the endpoints (cause 2: the TD pool). the owner
check: his voice and the dashboard sounds are in the earpiece.

**E3, reload with the headset streaming**, game audio still off (R1, R2 and T9 do real work;
no MEC client is registered anywhere now).

```bash
python3 xw.py reload --step --settle        # xw1 -> xw2
```

Expected: `xw1.xex: voice stage 4, audio stage 4, game audio enable 0, HUD enable 0`, then the
old build's gates R1 to R6 (`--- xw1.xex Rn next:`), each after these codes: R1 `REL_BEGIN`,
`REL_CHECKED`; R2 `REL_AUDIO_DRAINED`; R3 `REL_AUDIO_OFF`; R4 `REL_GAME_QUIESCED` (log `game:
quiesced (no MEC client registered)`); R5 `REL_PADS_RELEASED`, `REL_QUIESCED`,
`REL_XAM_DRAINED`, `REL_TX_DRAINED`; R6 `REL_COMMIT`, `COMMIT_BEGIN`, `COMMIT_UNLINKED`,
`COMMIT_DONE`, `REL_COMMITTED`; then `REL_XINPUT_QUIESCED`, `REL_DONE`, `xw1.xex: released`.
Then T1 to T7 as in E1, `T8 voice stage back to 4`, `T9 next: headset audio stage back to 4`
followed by `hsrestart port 0` and, within a few seconds, `AUD_OPEN port 0`, `AUD_OPENED
port 0`; T10 and T11 `stays 0`; `done: xw2.xex`. `xw.py status`: streaming, mic ~1000/s.

**E4, game audio** (cause 3: the second registration, then unregister and register).

```bash
python3 voice.py game on     # within ~1 s: voice.py status "game audio: on ... registered"
sleep 30                     # a freeze here: the second MEC registration ever, no reload
python3 xw.py reload --step --settle        # xw2 -> xw3
```

Expected as E3, plus: just before the R4 prompt, log `game: quiesced, MEC client ...
unregistered`; `T10 next:
game audio enable back to 1`, after which `voice.py status` says registered within ~1 s.

**E5, the title switch on purpose** (cause 1). The gates themselves delay the take-over past
the switch, so E5 runs without `--step`. First with `--settle` only, then with neither:

```bash
python3 xw.py reload --settle   # E5a: xw3 -> xw4, everything on, no gates
python3 xw.py reload            # E5b: xw4 -> xw5, the take-over word right at the load
```

In the tee, the new build's `title` change (the loader title replaced by Aurora) comes after
`TAKE_CMD` and the take-over's steps in E5b, and before them in E5a. E5a passing and E5b
freezing confirms cause 1; the tee's interleaving of `title` and trace lines shows which step
met the switch.

**E6, in place** (no title switch at all).

```bash
python3 xw.py reload --in-place --step --hold 5   # at Aurora: XexLoadImage from the old build's thread
python3 xw.py reload --in-place                   # then the owner starts a Halo 3 match: in the match
```

Expected: no `load` line; `xwN.xex: XexLoadImage status 00000000`; the old build's trace ends
`REL_LOAD_NEXT`, `REL_LOADED`. In the match, the owner check: the game shows the controller
disconnect and reconnect, and keeps running.

**E7, three in a row without touching the pad** (the forwarded-IN chain).

```bash
for i in 1 2 3; do python3 xw.py reload --settle || break; done
python3 xw.py trace          # every build: in_double 0
python3 xw.py handover       # N builds, the newest running
```

Then the owner presses a button: Aurora reacts.

After E1-E7: `accept/run.py p3_reload` is the Phase 3 acceptance (10 reloads; it needs about
40 s of build per reload).

**A1-A4, the audio freeze** ([Open](#open-audio-on-a-reloaded-build)). Fresh boot with this
build installed as `xw.xex`, pad on, headset in the jack and streaming (`xw.py status`: mic
~1000/s), game audio on, HUD at stage 0. Tee running. In each step the owner presses buttons
for 2 minutes, Share + D-pad included (balance), as when it froze. Stop at the first freeze;
it names the cause, and costs a reboot.

1. **A1, iso close and reopen in one build (a), no reload.** Also the first test of the
   headset resume:
   ```bash
   python3 xw.py audio 0 && python3 xw.py hsrestart 0   # audio off: AUD_CLOSE, AUD_CLOSED x2
   python3 xw.py audio 4 && python3 xw.py hsrestart 0   # resume with the known 09/10
   ```
   Expected after the second line, within ~2 s: `gip: client 0 headset: resuming with
   format 09/10 (no identify)`, `SET_INTERFACE 1/1`, trace `AUD_ALT`, `AUD_OPEN`,
   `AUD_OPENED`, `ISO_IN_FIRST`, `ISO_OUT_FIRST`, then `headset accepted` (or `streams
   with` / `silent: resuming with`) `09/10: power on` and `headset ready`; `xw.py status`
   mic ~1000/s. A freeze here is (a), with no reload involved.
2. **A2, the MEC client again (c), no reload:** `python3 xw.py mecreset`. Expected: trace
   `MEC_RESET`, log `game: quiesced, MEC client 801960ac unregistered`, `game: resumed`,
   trace `MEC_RESUMED`, then within ~1 s `game: MEC client registered (handle 801960ac ...)`
   and `game: tap copying`. A freeze here is (c).
3. **A3, a reload with everything on:** `python3 xw.py reload --settle` (in place). Expected:
   `take-over done: 1 adopted`; ~1.5 s after `pad 0: ready`, `headset: resuming with format
   09/10`, then headset ready and mic ~1000/s with no replug; the MEC client registers 20 s
   after xw1 started; `xw.py status` shows balance etc. as before. Then the 2 minutes of
   buttons. A freeze here, with A1 and A2 clean, is something reload-specific.
4. **A4, the HUD as on xw2:** `python3 hud.py stage 3`, 2 more minutes of Share + D-pad (the
   panel drawn on screen).

## Unverified

Known unknowns, most important first (none of these is checked on the console yet):

1. **Whether a system DLL loads at `0x92800000`.** `xa.xex`'s slots use `0x92000000`; whether
   that reload was ever run is not recorded. If `XexLoadImage` fails, the loader's `g_stage[3]`
   has the status and `xw.py reload` reports that the module did not load.
2. **GIP after an adoption.** The pad must answer an identify request while it is on, and
   accept a second security handshake. If the handshake fails, input still works (it is
   marked ready at once on a cable); the headset is asked for directly.
3. **Changed:** the headset does NOT answer an identify request once it runs (console, Nov
   22). It is resumed with its known format instead ([Headset after a reload](#headset-after-a-reload));
   whether it answers the format suggestion, or only the power-on, is not known yet (A1).
4. **`XAudioUnregisterRenderDriverMECClient`** works (called at three releases, Nov 22); whether
   the tap is safe after it and a new registration is the open question (A2). Its code
   is checked against the dump first.
5. **The class-driver swap at `node + 4`** is from disassembly, for kernel 17559 only.
6. **Verified (E1', Nov 22):** in place, `XexLoadImage` from the driver thread loads the next
   build (status 0); it is the default now. The loader title's path froze the console (E1).
7. **A USB reset on a lit pad** (`--reset`): not recorded anywhere whether it stays lit.
8. Titles' reaction to the unbind and rebind in the middle of a game.

## Test procedure

Console at the Aurora dashboard, pad and headset plugged in.

```bash
cd /Users/owner/Documents/code/Xbox360-GIP-Wired
bash toolchain/build.sh                       # must end with: out/xw.xex ... ok, no FP
cd xbox360
export XBOX=10.0.0.13 XW_DIR='Hdd:\Plugins'
```

**0. Install once (one reboot, one button press).**

```bash
python3 xw.py upload                          # Hdd:\Plugins\xw.xex and xw_loader_hdd.xex
```

Reboot the console, press the pad's Xbox button, wait 40 s (the voice and game-audio hooks
go in 20 s after start).

**1. Baseline.**

```bash
python3 xw.py status                          # state running, pad bound, headset streaming
python3 xw.py handover                        # xw.xex: handover v1 ..., slot 0, running
```

**2. First reload, at the dashboard, hands off the pad.**

```bash
python3 xw.py reload
```

Watch the pad's light the whole time: it must stay on. Expected output, in order: `replacing
xw.xex (running) with xw1.xex`, `building slot 1 ...`, two `upload` lines, `xw.xex: release`,
`xw.xex: released`, `load 200- ...` (the screen goes black, then Aurora comes back), `xw1:
take over from ...`, the new build's log, then `take-over done: 1 adopted, 0 reset, 0 left
alone, 7 hooks inherited` and `done: xw1.xex is the running build`.

In the new build's log, look for:

```
detour: 7 hook(s) inherited from the previous build
detour: XInputdReadState at ... adopted (slot ...)
reload: port 0: device ... adopted, no USB reset; waiting for the previous build's IN transfer
gip: client 0 taken over from the previous build: asking it to identify itself
pad 0: ready
gip: client 0: asking for the headset the previous build had     (or: a headset is in the jack)
```

**3. Check it works.** Press buttons (Aurora reacts), then:

```bash
python3 xw.py status                          # follows xw1 now: pad bound, counters moving
python3 xw.py handover                        # xw.xex released, xw1.xex running, 1384 KB
python3 voice.py status                       # headset bound again; game audio registered after 20 s
```

**4. Second reload without touching the pad** (the chain: the first build's patch, the
second build's IN transfer).

```bash
python3 xw.py reload                          # xw1 -> xw2
```

Then press a button. `xw.py handover` shows three builds.

**5. In a game, without leaving it** (last; the riskiest):

```bash
python3 xw.py reload --in-place
```

The game should see the controller disconnect and reconnect, and keep running.

**If it goes wrong.** Note the last line `xw.py reload` printed, then:

```bash
python3 xw.py handover
python3 xw.py status
```

A refused release leaves the old build running: read the reason and retry. A dark or dead pad
after a take-over: `python3 xw.py usbreset 0`, then replug, then reboot. A frozen console:
power-cycle; the last printed step says which stage froze it (after `release`: the old
build's teardown; after `load`: the module load; after `take over`: the adoption).
