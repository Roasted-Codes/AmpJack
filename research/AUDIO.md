# Chat audio through a wired GIP controller's headset jack

Research notes, Nov 2026. Offline analysis of the kernel 17559 / XAM images dumped from the owner's
console (`research/dumps/`), xone (Linux GIP driver), hiddriver360 and our own `xw.xex`. Nothing
here has been run on a console. Addresses are retail 17559 (`xboxkrnl.exe` at 0x80040000,
`xam.xex` at 0x815F0000). Disassembly excerpts are abridged; `k.dis`/`x.dis` were produced
with the `xw-toolchain` llvm-objdump as described in the task notes.

Scope: voice chat (mic in, earpiece out), sections 1-7. Game audio in the earpiece goes through
the XAudio render driver, a different path: section 8.

## TL;DR

- The 360 has a kernel "voice device" layer, **XVoiced**, exported from `xboxkrnl.exe`
  (ordinals 481-484, 640, 641, 807, 860). Titles reach it through thin XAM wrappers
  (`XamVoiceCreate` 780, `XamVoiceHeadsetPresent` 781, `XamVoiceSubmitPacket` 782,
  `XamVoiceClose` 783). Everything is keyed by the **XInput device tag** (the "device context"
  we already hand to `XamUserBindDeviceCallback`); a headset is a device bound in **category 1**
  for the same user. XVoiced is a format-agnostic byte pipe: it moves caller-owned buffers to and
  from the USB endpoint in 32-byte interrupt packets and sets a status word when done.
- A virtual headset for our pads therefore needs: (1) detours on `XVoicedHeadsetPresent`,
  `XVoicedSubmitPacket`, `XVoicedClose` that act only when `ctx[0]` is one of our tags
  (0x10000005+slot), exactly like the existing XInput hooks; (2) one extra
  `XamUserBindDeviceCallback(id, tag, category 1, ...)` when a headset appears on the pad.
  Nothing else in XAM needs touching. Confidence: high (traced end to end in XAM and the kernel;
  the only part not seen is the title-side XHV library, which has no other way in).
- Usbd on 17559 **does** support isochronous endpoints: `UsbdOpenEndpoint` accepts type 1 on
  OHCI (and EHCI), and `UsbdQueueIsochTransfer` is exported (ordinal 895, 0x800D82C0). The only
  kernel users are Kinect (high speed) and the camera; a full-speed isoch user on OHCI is new
  ground. Confidence: medium; the transfer-block layout and callback signature below are read from
  the code, not tested.
- GIP side (xone): the jack headset shows up as GIP **client 1** on the same interrupt pipe;
  audio is negotiated with command 0x08 and streamed as command 0x60 packets over **interface 1,
  alternate setting 1, isochronous IN/OUT**, 1 ms frames, s16le PCM, rate from a format table the
  controller advertises in its IDENTIFY reply. Our `core/gip.c` drops every packet whose header
  client id is non-zero, so it needs a per-client dispatch first. Confidence: high for the
  protocol (xone works), unknown for the SCUF's format list and whether audio needs the
  security handshake we skip.
- The stock headset's PCM rate/endianness is **not visible** in the kernel or XAM (both just move
  bytes); it is fixed by the XHV library in the title and the headset hardware. It has to be
  measured once on the console with a real wired 360 pad + headset (stage R2 below) before the
  resampler is written. Known USB descriptor facts suggest 16-bit mono at 8 or 16 kHz.
- Overall: feasible, in stages, with every console step gated. The risky step is the first isoch
  transfer on OHCI; everything before it can be proven with no USB traffic at all.

## 1. How a stock wired 360 pad's headset reaches games

### 1.1 USB side: interface FF/5D/03, interrupt transfers, 32-byte packets

A wired 360 pad is a composite device. Usbd offers **each interface** to class drivers: first the
runtime list at 0x801A8048 (walked by 0x800D6178, called from 0x800D62A4), then a built-in table
keyed by class/subclass/protocol (so our `xw.xex` driver really is asked about every interface,
and could claim the 360 pad's interfaces if it said yes; it declines non-FF/47/D0).

`UsbdSelectClassDriver`'s built-in dispatch, class 0xFF subclass 0x5D (0x800D62C0..0x800D6504):

```
800d62c0: cmplwi cr6, r27, 255      ; bInterfaceClass == 0xFF
800d62c8: cmplwi cr6, r26, 93       ; bInterfaceSubClass == 0x5D
800d62d0: cmplwi cr6, r28, 1        ; protocol 1  -> 0x801726E8 (wired gamepad driver)
800d62e4: cmplwi cr6, r28, 255      ; protocol FF -> 0x80172654
800d62f8: cmplwi cr6, r28, 129      ; protocol 81 -> 0x80172598 (wireless receiver)
800d630c: cmplwi cr6, r28, 4        ; protocol 4  -> 0x80172638
800d6320: cmplwi cr6, r28, 130      ; protocol 82 -> 0x80172954
800d64bc: cmplwi cr6, r28, 3        ; protocol 3  -> 0x80172980 (headset)
800d64f4: cmplwi cr6, r28, 2        ; protocol 2  -> 0x801725F0 (expansion/plug-in module)
800d6508: class 1 / subclass 1 (USB Audio Control) -> 0x8017C4B8 (Kinect audio)
```

The headset entry at 0x80172980 is `{flink 0, blink 0, flags 0x20000, AddDevice 0x801069F8,
RemoveDevice 0x80106C08, match NULL, after NULL}`. Its AddDevice (0x801069F8):

- `UsbdIsDeviceAuthenticated(node, 2)` or `(node, 0x82)` must succeed, otherwise
  `UsbdAddDeviceComplete(node, 0xC0051012)`: the headset interface is refused unless the pad's
  security interface (FF/FD/13) authenticated. (Irrelevant for us; we never use this driver.)
- Finds the gamepad object for the same device (0x80106300), zeroes 436 bytes at obj+60, then
  looks up **four interrupt endpoints** with `UsbdGetEndpointDescriptor(node, index=i>>1,
  type=3, dir=~i&1)`, i = 0..3 (IN0, OUT0, IN1, OUT1), and opens them with maxpacket 32:

```
80106b8c: addi r8, r27, -81          ; transfer block for endpoint i (92-byte slots from obj+92)
80106b90: lbz  r7, 0(r27)            ; bInterval (copied from the descriptor at obj+173+92i)
80106b94: li   r6, 32                ; wMaxPacketSize 32
80106b98: lbz  r5, -1(r27)           ; bEndpointAddress (obj+172+92i)
80106b9c: li   r4, 3                 ; USBD_TYPE_INTERRUPT
80106ba4: bl   UsbdOpenEndpoint
```

- Then queues a **4-byte interrupt IN** on endpoint index 2 (the second IN, the 360 pad's
  0x83 "status" endpoint) into a 64-byte buffer at obj+460. Its completion (0x80107328) parses
  `buf[0]` (1 = headset status, 5 = ...) and sets the **headset-present byte at obj+0x54**
  (`stb r8, 84(r31)` at 0x80107484 / 0x8010767C), then binds or unbinds the headset in XAM
  (section 1.3).

Audio moves on endpoint index 0 (IN, mic) and 1 (OUT, earpiece) in chunks of at most 32 bytes
(0x801070A8: `cmplwi r8, 32; li r8, 32`), each chunk a normal `UsbdQueueAsyncTransfer` whose
completion adds the block's actual length (block+0x1C, `lwz r9, 124(r30)` at 0x8010722C, which
also confirms the "Usbd writes the actual length at +0x1C" rule from `xw_plugin.c`) to the
packet's byte count. **No isochronous transfers are involved in the stock headset path.**

Format: not in the kernel. The driver never looks at the bytes. Public descriptor dumps of
045e:028e put interface 1 at FF/5D/03 with EP 0x82 IN (32 bytes, bInterval 2), 0x02 OUT
(32 bytes, bInterval 4), 0x83 IN (interval 64) and 0x03 OUT (interval 16); 32 bytes every 2 ms
of 16-bit mono is 8 kHz, but the device may answer every 1 ms poll. Treat the rate and the byte
order as **unknown until measured** (stage R2).

### 1.2 Kernel: XVoiced, keyed by device tag

Exports (from the kernel's export directory, resolved at RVA+0x80040000):

| ord | address | name | notes |
|---|---|---|---|
| 481 | 0x80101C40 | `XVoicedHeadsetPresent(ctx)` | returns byte dev+0x54 |
| 482 | 0x80102048 | `XVoicedSubmitPacket(ctx, dir, pkt)` | queues a packet |
| 483 | 0x80102230 | `XVoicedClose(ctx)` | cancels ctx's packets, waits |
| 484 | 0x80101DC8 | `XVoicedActivate(bool)` | sets global 0x80188048 |
| 640 | 0x80101E30 | `XVoicedSendVPort(tag, port)` | only for tags 0x4/0x5 |
| 641 | 0x80101E80 | `XVoicedGetBatteryStatus(tag, out)` | only 0x4/0x5, else 4319 |
| 807 | 0x80101CF8 | `XVoicedGetDirectionalData` | only tag 0x7 (Kinect) |
| 860 | 0x80101DE8 | `XVoicedIsActiveProcess()` | process type == activated |

Routing is by the **top nibble of `ctx[0]`**, which is the XInput device tag:

```
80101c4c: lwz   r3, 0(r3)                 ; tag = ctx[0]
80101c58: rlwinm r11, r3, 0, 0, 3         ; top nibble
          accepted: 0x9, 0x1, 0x2, 0x4, 0x7 ; anything else -> return 0 (no headset)
80101cbc: bl 0x80102918  ; 0x1/0x4 : wireless table, entry = 0x80188050 + (tag & 0x0FFFFFFF) * 1440
80101cc4: bl 0x80101C28  ; 0x9     : 0x80187EE8 + idx * 88
80101ccc: bl 0x800FDA38  ; 0x7     : single object 0x801AA680 (Kinect, USB Audio, isoch)
80101cd4: bl 0x801065B0  ; 0x2     : wired 360 pads, 0x801896D8 + idx * 496
80101ce0: lbz   r3, 84(r3)                ; headset present byte
```

(Same dispatch as `XInputdGetDeviceStats`' 0x1/0x2 routing; the 1440-byte table is the
wireless-controller table, four entries 0x80188050..0x801896D0, immediately followed by the
wired table at 0x801896D8.)

The voice sub-object inside each device object: +0x00 KDPC (cancel), +0x1C KDPC (start
transfers; `KeInsertQueueDpc(dev+28)` at 0x801021D8), +0x38 spinlock, +0x44 render (OUT) queue,
+0x4C capture (IN) queue, +0x54 headset-present byte.

`XVoicedSubmitPacket(ctx, dir, pkt)` (0x80102048):

- `ctx`: 56 bytes, allocated by XAM. `ctx[0]` = device tag, `ctx+4` = node count, `ctx+8` =
  node array, `ctx+12/+16` = free-node list (16-byte nodes: `{pkt, event?, ctx, next}`),
  `ctx+20` = KEVENT (used by Close).
- `pkt`: `[0]` status (set to 0x103 STATUS_PENDING on submit; becomes 0 on completion,
  0xC0000001/0xC0000120 on cancel), `[4]` bytes transferred (set to 0, advanced per USB
  chunk), `[8]` buffer, `[12]` length, `[16]` zeroed (unknown use).
- `dir != 0` → capture queue (dev+76, endpoint index 0 = mic IN); `dir == 0` → render queue
  (dev+68, endpoint index 1 = earpiece OUT). (0x80102148..0x80102188 and the completion at
  0x80107140: `cmpwi r23,0 → dev+76; cmpwi r23,1 → dev+68`.)
- Errors: no headset (byte +0x54 zero) → 0xD000009D; no free node → 0x800700AA; process not
  the activated one (`XVoicedIsActiveProcess` false) → the packet is completed immediately
  (`pkt[4] = pkt[12]`, `pkt[0] = dir ? 0x80000011 : 0`) and 0 is returned.
- Completion is **polled**: the kernel only writes `pkt[4]` then `pkt[0]` (0x80107238,
  0x801072BC); the node's event field is always NULL for this driver. XHV polls the status.

### 1.3 XAM: which device a user's headset is

XAM keeps per user a table of 8 device **categories** (`XamUserGetDeviceContext(user, category,
&tag)` = 0x816D8A58: `entry = *(0x81A82FF4) + 8 + (user*8 + category)*8`, `{bound, tag}`). Our
pads are category 0. On every bind/unbind XAM calls a per-category callback from the table at
0x81A82FF8:

```
cat 0: 816F27F8   cat 1: 816FC228 (voice)   cat 2: 816F2890   cat 3: 816F2850
cat 4: 816FB5B8   cat 5: 816D8C78           cat 6: 816FC9D0
```

The category-1 callback 0x816FC228 copies the user's category-1 tag into XAM's voice table
(0x81AACA18+16+user*56 for the title process, +224 for the system/Guide process), sends
`XVoicedSendVPort(tag, user+1)` for non-Kinect tags (the kernel ignores it for tags 0x1), and
posts the "headset connected" notification (`0x816A93C8(0x8001F, ...)`). On unbind it zeroes the
tag (0x816FC3B4..0x816FC3C0).

`XamVoiceCreate(user, packets, &ctx)` (0x816FC098) takes the tag from that voice table
(`lwzx r26, ...; stw r26, 0(r31)` at 0x816FC0E4/0x816FC124), allocates the nodes and returns the
ctx. `XamVoiceHeadsetPresent(ctx)` → `XVoicedHeadsetPresent(ctx)`; `XamVoiceSubmitPacket(ctx,
dir, pkt)` → `XVoicedSubmitPacket`; `XamVoiceClose(ctx)` → `XVoicedClose` (0x816FB418,
0x816FB498, 0x816FC1C8). These wrappers do nothing else (they call `KeGetCurrentProcessType`
and ignore the result).

The kernel's wired driver is what binds the headset as category 1, with the **same controller id
as the gamepad** and the gamepad's tag:

```
801064a4..801064cc: tag = 0x20000000 | ((obj - 0x801896D8) / 496)
                    XamUserBindDeviceCallback(id = r31 (also stored at obj+488), tag, category 1, 0, NULL)
80106560..80106588: ... (id = obj+488, tag, category 1, disconnect 1, NULL)     ; on removal
```

(0x8012BBD0 is the kernel wrapper that loads the bind entry at 0x80189EB0 = table +0x10, the entry
`xinput_360.c` already checks.) The wireless driver does the same with its 0x1 tags
(0x80105CF8, 0x8010782C). XAM's bind code (0x816D8D84..0x816D8E5C) finds the user whose entry
already holds that controller id, so the headset lands on the pad's user.

So the association chain is: pad bound (category 0, tag T) → headset bound (category 1, same
id, tag T) → XAM voice table[user] = T → title's `XamVoiceCreate` puts T in `ctx[0]` →
`XVoiced*` routes on T.

Side note on today's `xw.xex`: our tags 0x10000005..8 would index **past** the wireless table in
`XVoicedHeadsetPresent` (entry 6 happens to hold 0x3F800000 floats, so it would read "present"
and then spin on a float as a spinlock). This is only reachable if our tag is ever bound in
category 1, which nothing does today, so there is no present-day hazard; but the detours in
section 2 must be installed **before** the first category-1 bind.

## 2. The virtual headset: what to hook and what to call

Least invasive design, mirroring the XInput hooks:

1. **Detour three kernel exports** (same `detour.c` mechanism, NV64 thunks):
   - `XVoicedHeadsetPresent(ctx)`: if `ctx[0]` in [CONTEXT_BASE, CONTEXT_BASE+3] → return our
     per-slot `headset_present` byte; else trampoline.
   - `XVoicedSubmitPacket(ctx, dir, pkt)`: ours → mimic the kernel: if not
     `XVoicedIsActiveProcess()` (ordinal 860, a plain function, safe to call) complete at once
     (`pkt[4] = pkt[12]; pkt[0] = dir ? 0x80000011 : 0`; return 0); if no headset → return
     0xD000009D; else set `pkt[0] = 0x103, pkt[4] = 0, pkt[16] = 0`, append to our per-slot
     capture/render ring (16 entries; full → 0x800700AA), return 0.
   - `XVoicedClose(ctx)`: ours → complete every pending packet of that ctx with 0xC0000120 and
     return. (The kernel version waits on `ctx+20`; we never use XAM's nodes or event, so nothing
     to wait for.)
   - `XVoicedGetBatteryStatus`/`SendVPort`/`GetDirectionalData` already reject or ignore 0x1 tags;
     leave them alone.
   Note XAM keeps a separate ctx per user for the title process and the Guide process; the hook
   keys on `ctx[0]` + the ctx pointer, not on the user, so both work.

2. **Bind the headset**: when the pad reports a headset (GIP client 1 identified as
   `Windows.Xbox.Input.Headset`), from the XAM thread:
   `pXamUserBind(CONTROLLER_ID_BASE + slot, CONTEXT_BASE + slot, 1 /*category*/, 0, NULL)`;
   on removal the same with disconnect = 1. XAM then shows the headset icon, records the tag,
   and titles start calling `XamVoiceHeadsetPresent`. Unbind before the pad itself is unbound.

3. **Serve packets** from the driver thread/DPC: capture packets are filled from the GIP audio
   IN stream (converted), `pkt[4]` advanced, `lwsync`, then `pkt[0] = 0` when full; render
   packets are drained into the GIP audio OUT stream at its pace and completed when consumed. If
   XHV has no render packet pending, send silence; if no capture packet is pending, drop mic
   frames. Pacing comes from the isoch frame clock (1 ms), which is what XHV expects from the
   USB-paced stock driver.

Everything in (1)-(3) except the GIP stream can be exercised with **no USB traffic**: a
"silent virtual headset" that completes capture packets with zeros at a fixed byte rate proves
the whole XAM/XHV plumbing.

Unknowns: whether XHV tolerates packets completing in bursts larger than 32 bytes (likely yes,
it only reads `pkt[0]`/`pkt[4]`); whether some titles use `XamUserGetDeviceContext(user, 1, ...)`
directly (harmless: it returns our tag, which routes to the same hooks).

## 3. Usbd isochronous support on 17559

- `UsbdOpenEndpoint(node, type, addr, maxp, interval, block)` (0x800D81A0) dispatches on the
  controller (OHCI 0x800DC310, EHCI 0x800DEC80). The OHCI implementation accepts type 1:

```
800dc378: cmplwi cr6, r30, 1
800dc37c: blt  -> 0x800dc3e4   ; type 0 (control): async list (0x800DBF38)
800dc380: beq  -> 0x800dc3b8   ; type 1 (isoch): li r26, 1 (interval forced to 1 ms) then periodic insert
800dc384: cmplwi cr6, r30, 3
800dc388: blt  -> 0x800dc3e4   ; type 2 (bulk): async list
800dc38c: beq  -> 0x800dc3bc   ; type 3 (interrupt): interval rounded down to a power of two <= 32, periodic insert (0x800DC170)
800dc390: lis r3, 0xC000; ori r3, r3, 13   ; else STATUS_INVALID_PARAMETER
```

  The ED builder 0x800DC098 folds `(type == 1)` into the ED's control word (the F bit) and stores
  `type & 3` at ED+24.
- `UsbdGetEndpointDescriptor(node, index, type, dir)` works with type 1: the Kinect audio driver
  calls it as `(node, 0, 1, 1)` (0x800FDC14..0x800FDC28) and then opens
  `UsbdOpenEndpoint(node, 1, addr, 76, 1, block)` / `(node, 1, addr, 524, 1, block)`
  (0x800FDD2C, 0x800FDD5C). Whether it returns the endpoints of **alternate setting 1** (GIP
  audio lives in alt 1; alt 0 has none) is not proven. hiddriver360 uses the same call for
  interrupt endpoints; xone's data interface is alt-less. Stage U1 checks this read-only.
- `UsbdQueueIsochTransfer(node, block, u16 *frame_lengths)` (ordinal 895, 0x800D82C0; OHCI
  implementation 0x800DC9A8). The block layout differs from the async one:
  `+0x04` callback, `+0x08` saved endpoint, `+0x10` buffer, `+0x14` total length,
  `+0x18` **byte**: frame count, `+0x1C..` the caller's per-frame length array (the Kinect driver
  passes `block+28` and fills 16 entries, 0x800FE54C..0x800FE574). The OHCI code builds one iTD:
  `MmLockUnlockBufferPages(buffer, len, 0)`, start frame from the ED's bookkeeping (ED+20 next
  frame, ED+22 pending count) or the current frame, one PSW per frame (0xE000 | offset), FC =
  frames-1, BP0/BE from `MmGetPhysicalAddress`. An OHCI iTD has **8** PSW slots, so a full-speed
  block must carry ≤ 8 frames (xone uses exactly 8 per URB). The 16-frame Kinect block goes
  through EHCI.
- Completion (0x800DCCE0 → 0x800D5D20): per frame, PSW condition code → NTSTATUS via the table
  at 0x800447F8 and length (0 if "not accessed"), then
  `MmLockUnlockBufferPages(buffer, len, 1)` and **`callback(block, long *status_per_frame,
  u16 *length_per_frame)`** — three arguments, unlike the async `callback(block, status)`
  (0x800D5CC0). The arrays live on Usbd's stack; copy what you need inside the callback.
- Buffers: Usbd locks/translates pages itself; plain image memory worked for all our transfers so
  far and the Kinect driver uses static buffers too. Keep one block's buffer inside one page
  (8 frames × ≤ 256 bytes fits).
- The GIP pad is full speed, so it must be on a root port (OHCI companion). Behind a high-speed
  hub Usbd would need EHCI split isoch, which may not exist; don't test through a hub.

What a GIP-audio-without-isoch alternative looks like: none on the controller side (the audio
interface only has isoch endpoints). If isoch proves unusable, chat audio through the pad is
infeasible; the fall-back would be a different device for audio (not pursued here).

## 4. GIP side (from xone)

Attachment and discovery (`bus/protocol.c`, `driver/headset.c`):

- Packets carry a **client id in the low nibble of the options byte** (`GIP_HDR_CLIENT_ID =
  options & 0x0F`). The controller is client 0; a headset in the jack is client 1 (chatpad 2).
  `gip_process_buffer` creates/looks up a client per id and dispatches per client. Our
  `gip_rx` (`core/gip.c:866`) drops everything with a non-zero id, and our acknowledge/identify/
  power/format senders must put the client id in `options` (`hdr.options = client->id |
  GIP_OPT_INTERNAL`).
- When a headset is plugged into the jack the pad sends, with client id 1: ANNOUNCE (0x02) →
  we send IDENTIFY (0x04, options 0x21) → IDENTIFY reply: the element table at offset 16 has the
  **audio formats** at `le16 @ data+20` (our parser only reads classes at +26): `count`, then
  `count` pairs `{in, out}` of `gip_audio_format` (1 = 8 kHz mono, 2 = 8 kHz stereo, 3/4 = 12 kHz,
  5/6 = 16 kHz, 7/8 = 20 kHz, 9/10 = 24 kHz, 11/12 = 32 kHz, 13/14 = 40 kHz, 15/16 = 48 kHz;
  odd = mono). Class string `Windows.Xbox.Input.Headset`.
- Enabling audio (`gip_headset_probe`/`gip_headset_config`/`gip_headset_register`):
  1. `SET_INTERFACE(1, 1)` (control, wIndex 1, wValue 1). xone sets alt 0 first at probe
     ("mandatory for certain third party devices"); `xw_plugin.c` already does that.
  2. POWER (0x05) subcommand `GIP_PWR_SLEEP` to the headset client, then AUDIO_CONTROL (0x08)
     subcommand 0x02 FORMAT `{in, out}` using the first advertised pair. The device echoes
     0x08/0x02; a different pair means "use this instead" (re-suggest). Microsoft's 045e:0111
     chat headset uses subcommand 0x01 `{12 kHz stereo}` instead.
  3. POWER (0x05) `GIP_PWR_ON` every 500 ms until a volume report (0x08/0x03) or the first
     audio packet arrives (max 6 tries), then for jack headsets `AUDIO_CONTROL 0x03 VOLUME
     {mute 0x04 (unmuted), out 100, chat 50, in 100, 0, 0, 0}`.
  4. Start isoch IN (8 frames of maxpacket) and OUT.
- Streaming: `GIP_AUDIO_INTERVAL` = 8 ms. `buffer_size = rate * channels * 2 * 8 / 1000` bytes per
  8 ms (16 kHz mono: 256), `fragment = buffer_size / 8` (32), one GIP packet per 1 ms frame:
  header `{0x60, client_id | 0x20, sequence (1..255, never 0), length = fragment}` + samples
  (16 kHz mono: 36-byte frames). IN frames are the same framing from the device with a 2-byte
  `le16 flow_rate` before the samples (`gip_pkt_audio_samples`); decode them with the normal
  header parser. Sequence is per adapter, not per client.
- Standalone headsets need the auth handshake before audio; jack headsets on a pad do not in
  xone. Whether the **pad** gates jack audio on its own (skipped) authentication is untested;
  the Mac tool answers it (stage G0).

## 5. Format conversion and timing

- GIP: s16 **little-endian**, interleaved, rate per negotiated format. Choose the lowest mono
  rate the pad offers that is an integer ratio of the XHV rate once measured (8 kHz mono if
  offered and XHV is 8 kHz; 16 kHz mono otherwise).
- 360 side: 16-bit PCM, byte order unknown (probably LE, it comes straight from a USB device;
  stage R2 dumps a real capture buffer to settle it).
- Conversions needed are all integer and small: byte swap if needed; 2:1 (16k→8k: average
  pairs, or a 4-tap integer FIR), 1:2 (8k→16k: linear), 3:2 (24k→16k) and 3:1 (24k→8k,
  48k→16k) polyphase with 16-bit coefficients and 32-bit accumulators. Stereo→mono: average.
  No floating point anywhere, so the work can run in the isoch completion (DPC) or on the driver
  thread; per frame it is ≤ 48 samples. Prefer the driver thread (ring between DPC and thread,
  as for input) and keep the DPC to copying frames and re-queueing.
- Latency budget: GIP 8 ms blocks + one block of slack ≈ 16-24 ms each way, well under what XHV's
  network path adds. Clock drift between the pad's 1 kHz frame clock and XHV's consumption: XHV
  is paced by completions, so no drift handling is needed for capture; for render, if XHV falls
  behind, send silence; if it runs ahead, packets simply queue (bounded by the ring).

## 6. Staged plan

Principles: (a) the Mac tool proves every GIP step before the console sees it; (b) on the console
each stage is a `go` gate in the XW36 status block (`xw.py go N`) with a counter and last
status, as `XW_STAGED` does now, so a freeze pins one step and a reboot without the flag
recovers; (c) read-only observation first, then hooks that only count, then hooks that act,
then USB.

**G0 - desktop, no console.** Extend `desktop/xwired`: dispatch by client id, log client-1
announce/identify (print the audio format pairs and class), implement 0x08 format/volume, power
retries, `SET_INTERFACE(1,1)`, libusb isoch IN/OUT, and a mic→earpiece loopback. Proves: the SCUF
exposes the headset, which formats it offers, whether audio works without the auth handshake,
packet layout. Needs the user: SCUF + SteelSeries on the Mac. Risk: none.

**R1 - console, read-only (user plugs in a real wired 360 pad + headset).** With a voice-capable
title or the Guide running, xbdm `getmem`: wired table 0x801896D8 + n·496: +0x54 present byte,
endpoint addr/interval pairs at +172/+173, +264/+265, +356/+357, +448/+449, status buffer pointer
at +460; XAM voice table 0x81AACA28 + user·56 → tag; `XamUserGetDeviceContext` table
(`*(0x81A82FF4)`). Also confirm `match_calls` ≈ 2 per GIP pad (interface 1 is offered to us).
Proves the model in 1.1-1.3 on this console. Risk: none.

**R2 - console, counting hooks (gated).** Detour `XVoicedSubmitPacket` and
`XVoicedHeadsetPresent` with pass-through bodies that only log `(tag, dir, pkt[12], time)` and,
after completion, the first 32 bytes of a capture buffer into the status block. With the real
pad + headset in a chat session this yields the byte rate (= sample rate) and byte order.
Risk: the detour install itself (same mechanism as the four existing hooks; the functions
start with `mflr r12; stw r12,-8(r1)` / `mflr r12; bl __savegprlr`, which `detour.c` already
handles). Gate: hooks are only installed at `go ≥ N`; nothing calls them until a title uses voice.

**R3 - safety hooks (gated).** Same detours, now acting for our tags: HeadsetPresent → 0,
SubmitPacket → "no headset", Close → ok. Expect no visible change. Proves the branch on `ctx[0]`
never disturbs stock devices.

**V1 - silent virtual headset (gated, no USB).** xbdm command binds category 1 for slot 0;
HeadsetPresent → 1; capture packets completed with zeros at the R2 byte rate from the driver
thread; render packets completed as "consumed" at the same rate. Expect the headset icon in the
Guide and, in a chat lobby, the title treating the pad as having a headset (other players hear
silence). Fail-safe: a flag to drop HeadsetPresent to 0 and an unbind command. Proves the entire
XAM/XHV path with zero Usbd exposure. This is the stage that tells whether any title-side surprise
exists before investing in isoch.

**U1 - interface 1 node, read-only (gated).** `XwMatchC` also accepts interface 1 of a pad we
already own; its AddDevice only stores the node and logs `UsbdGetEndpointDescriptor(node, 0, 1,
dir)` for both directions (does it see alt-1 isoch endpoints?). Then, gated, `UsbdOpenEndpoint`
type 1 for both and log the status; nothing queued. Then, gated, close them again. Risk: a bad
open status is harmless; a freeze here would be inside open/close only.

**U2 - first isoch IN (gated).** `SET_INTERFACE(1,1)` + GIP format/power on the data pipe (from
G0), then **one** `UsbdQueueIsochTransfer` of 8 frames into a page-aligned buffer, callback copies
`status[8]`/`len[8]` into the status block and does **not** re-queue. Inspect. Then re-queue from
the callback (continuous IN), then two alternating blocks. Then OUT with GIP silence frames. Each
a separate gate. Risk: this is the unknown ground (first third-party isoch on OHCI); the
transfer-block layout in section 3 is read from the kernel, not tested, which is why one
non-re-queued block comes first.

**A1 - connect.** Mic frames → resampler → capture packets; render packets → resampler → OUT
frames. Volume commands; headset removal → unbind; pad removal → disable audio first.

Needs the user: a real wired 360 pad with a headset (R1/R2), a voice-capable title or a party
chat session, the SCUF + SteelSeries on the Mac (G0), and the 045e:0b12 pad for the same tests.

## 7. Feasibility and open doubts

- XAM/XHV plumbing: high confidence; every link is in the dumps. Not seen: the XHV library inside
  titles (no title dump). If a title turned out to bypass `XamVoice*`, V1 would show it.
- Usbd isoch: medium. Supported and used by Microsoft's own drivers, but on OHCI only by inference
  from the code; the 8-frame limit, `+0x18` frame-count byte and the 3-argument callback are read,
  not tested. U1/U2 are designed so that each uncertainty fails in a bounded way.
- GIP audio on the SCUF: medium-high; xone's path is well exercised on Microsoft pads, third-party
  pads vary (xone has quirks for some). G0 answers it off-console.
- Sample rate / byte order of the 360 side: unknown; R2 measures it. If XHV turns out to need
  8 kHz and the pad offers only 16 kHz mono, the 2:1 conversion is trivial.
- Effort: G0 + hooks + V1 are days; isoch and the audio engine are the larger part. Chat audio is
  feasible; nothing found is a hard blocker, but the console has frozen twice before on transfer
  block details, and isoch adds a new block layout, so the gated order above is not optional.

## 8. Game audio

Goal: the console's final game mix, copied into the earpiece under chat, TV output untouched.
Implemented in `xbox360/game_360.c` (Nov 2026). Read offline from the 17559 dump with capstone
(VMX128 loads/stores decoded by hand: capstone doesn't know them); compared with Xenia's
`xboxkrnl_audio.cc`, `apu/audio_system.cc`, `apu/conversion.h`. Nothing here has run on a
console yet. "Verified" = read in the dump; "inferred" = from Xenia or reasoning only.

### 8.1 Exports (verified)

Resolved from the kernel's export table; the ordinals match Xenia's `xboxkrnl_table.inc`, and
the same table gives the already-proven XVoiced 481-483/860 and `KeSetBasePriorityThread` 153
at the addresses we use, so the numbering is the 17559 one.

| ord | address | name | body |
|---|---|---|---|
| 498 | 0x8012CA80 | `XAudioRenderDriverInitialize` | |
| 499 | 0x8012E250 | `XAudioRegisterRenderDriverClient(cb, &handle)` | 0x8012D640 |
| 500 | 0x8012E270 | `XAudioUnregisterRenderDriverClient(handle)` | 0x8012DA00 |
| 501 | 0x8012D598 | `XAudioSubmitRenderDriverFrame(handle, frame)` | 0x8012CE70 |
| 502 | 0x8012CC70 | `XAudioRenderDriverLock` | |
| 510 | 0x8012CE30 | `XAudioGetRenderDriverThread` | returns driver+0xC4 |
| 800 | 0x8012E288 | `XAudioRegisterRenderDriverMECClient(cb, &handle)` | 0x8012D828 |
| 801 | 0x8012E2A8 | `XAudioUnregisterRenderDriverMECClient(handle)` | 0x8012DBA0 |
| 802 | 0x8012D5B8 | `XAudioCaptureRenderDriverFrame(handle, buffer)` | 0x8012CFD0 |
| 879 | 0x8012CC18 | `XAudioSetProcessFrameCallback(fn)` | stores driver+0x19C |

Each export is a stub that loads the driver object **D = 0x80195F38** (0x8019A3D0 -
0x4498) into r3, shifts the arguments and branches to the body.

### 8.2 The render driver thread (verified)

Created at 0x8012E95C: `ExCreateThread(&D->thread (D+0xC4), 64 KB stack, ..., start 0x8012E3C0,
D, flags 0x10000083)` (affinity byte 0x10: hardware thread 4), then `KeSetPriorityThread(thread,
25)` (ordinal 163, real-time priority). Its loop 0x8012E3C0..0x8012E7BC, per frame:

1. `KeEnterCriticalRegion` (95): kernel APCs off; the thread stays at **PASSIVE_LEVEL**.
2. Mix buffer = `*(0x80195F00 + 0x30)` (0x8012F9D8 → 0x80130780), cleared with `dcbzl`, 0x1800
   bytes.
3. For each of the 8 client slots at D+0x40 (16 bytes: callback, arg, u16 process type at +8,
   suspended byte at +0xA, -1 at +0xC): take D's lock (D+0x30: `KeRaiseIrqlToDpcLevel` +
   `KeAcquireSpinLockAtRaisedIrql`, recursive by owner), set D+0xC0 = slot, **release the lock
   and lower IRQL**, then `callback(&local{arg})` (r3 = pointer to a stack word holding arg,
   as Xenia's "wrapped" argument). Titles' XAudio2 and XAM submit their frame from there.
4. 0x8012D158(D): the master fade/mute state machine (uses FP; kernel code, not ours).
5. If D+0x19C (`XAudioSetProcessFrameCallback`) is set: `fn(mix, 0x1800)`. Then an optional
   debug hook (`*(*0x8018CDA4)(mix, 0x1800)`, gated by 0x80170000).
6. For each of the **2 MEC slots at D+0x174** (same 16-byte layout): lock, D+0x194 = slot,
   unlock, `callback(&local{arg})` (0x8012E5A4..0x8012E6A0). No suspended check here.
7. D+0xC0 = D+0x194 = 0, frame counter D+0x198++, timing stats (mftb), an optional notifier
   (`*(0x80195758)+0x18`, (0x5B, mix)), `KeLeaveCriticalRegion`, then 0x8012F9E8 → 0x801307C0
   hands the mix to the hardware path, and loop. (The wait that paces the loop is somewhere in
   that hand-off; not traced. Inferred from Xenia: one frame per 256 samples, 187.5 Hz.)

`XAudioSubmitRenderDriverFrame` (0x8012CE70): the handle must lie in [D+0x40, D+0xC0), the
caller must be the render thread (`D+0xC4 == KPCR+0x100`) and the handle must be the slot being
serviced (`D+0xC0`); else 0x80070057. Then **mix += frame** for 0x600 × 4 bytes (96 iterations
of 64 bytes, `vaddfp`, the frame read with `lvlx`/`lvrx` so it may be unaligned). So the final
mix is the element-wise float sum of all client frames.

`XAudioCaptureRenderDriverFrame` (0x8012CFD0): the same checks against the MEC range
[D+0x174, D+0x194) and D+0x194, then a plain copy mix → buffer of 0x1800 bytes (`lvx128` from
the mix, `stvx128`/`stvx` to the buffer: **the buffer must be 16-byte aligned**), returns 0.

MEC register (0x8012D828): callback struct non-NULL with non-NULL callback; a title process
(`KeGetCurrentProcessType` 102 == 1) is refused if D+0x16C bit 0; first free of the 2 slots,
else 0x8007000E; copies {callback, arg}, stores the process type at +8 and the handle (= slot
address) to `*handle`, all under D's lock. Unregister (0x8012DBA0), called off the render
thread, spins (`NtYieldExecution`) while D+0x194 is that slot, then clears it: it waits for a
running callback. On title exit (0x8012E380) the kernel unregisters MEC slots whose type is 1.

### 8.3 Frame layout

- 0x1800 bytes = 1536 float32 = **256 samples × 6 channels** (verified: the copy and add
  lengths; floats: `vaddfp`).
- Big-endian (verified implicitly: plain loads on a big-endian CPU, and Xenia byte-swaps).
- Channel-sequential (planar), channel order FL FR C LFE SL SR: **inferred** from Xenia
  (`sequential_6_BE_to_interleaved_*`: `in[channel * 256 + sample]`, LFE index 3 skipped in its
  downmix). The kernel only sums client frames, so the layout is whatever XAudio2 writes; Xenia
  runs real games with this reading.
- 48 kHz: inferred (Xenia; 256 samples per 5.33 ms).

### 8.4 Choice of tap

| option | risk |
|---|---|
| Detour `XAudioSubmitRenderDriverFrame` | patches hot code on a real-time thread; sees each client's frame separately (title + XAM), not the mix; sums would be ours |
| `XAudioSetProcessFrameCallback` | one global slot, possibly owned by someone; we'd overwrite it |
| read the hardware ring (0x801307C0 path) | undocumented format and timing, not traced |
| **MEC client + `XAudioCaptureRenderDriverFrame`** | none patched; kernel's own API for the finished mix (Kinect's echo-cancellation reference); runs after every client and the process callback |

Chosen: the MEC client. Context of our callback: the render thread, priority 25, hardware
thread 4, PASSIVE_LEVEL in a critical region, **no kernel lock held** (the loop releases D's
lock before the call). A memcpy is safe there; the real constraint is time (the thread must
finish a frame within 5.33 ms) and never blocking. So the callback does: counter, early return
unless enabled with an earpiece being fed, `Capture` into a 128-byte-aligned static buffer,
integer conversion and downmix of 256 samples into a lock-free single-producer ring (8192
stereo frames), publish. No locks, no calls besides `Capture`, no logging.

Gating: registered on the driver thread 20 s after start, only if game audio is on, the two
export addresses match, and 46 words of the export stubs, the register body, the capture body
(range checks and the 0x600 length) and the render loop's MEC call site match the dump; else
refused for good. A failed registration (both slots in use) is retried every 10 s. Never
unregistered (xw.xex is never unloaded).

### 8.5 Conversion, drift, mixing (game_360.c)

- float → s16 with integer ops on the raw bits: exponent < 112 → 0; e ≥ 128 or ∞ → ±2.0 clamp;
  NaN → 0 (counted); else `(mantissa | 1<<23) >> (135 - e)` = value × 32768.
- Downmix, Q14: `L = 0.4142 FL + 0.2929 (C + SL)`, `R = 0.4142 FR + 0.2929 (C + SR)` (0.707
  ratio, normalised so that full scale on every channel can't clip), LFE dropped, clamped.
- Driver thread (`xv_earpiece_fill` → `xg_mix`): linear-interpolation resampler 48 kHz → the
  earpiece rate (Q20 phase), with a 2-tap pre-average when decimating (≤ 32 kHz). Drift: the
  ring fill (sampled per block, averaged over ~0.5 s) is steered to 45 ms by trimming the ratio
  proportionally, ±1000 ppm at ±15 ms error (1000 ppm = 1.7 cents, inaudible). Offline test
  (host build of game_360.c, 120 s, ±1.5 ms block jitter): ±300 ppm clock offset tracked, fill
  within ±5 ms of target, no underruns, no sample step above a clean sine's.
- Start: the ring is emptied (stale audio never plays), filled to 45 ms, faded in over 256
  frames. Underrun or > 120 ms full: fade the last sample out over the block, start again.
  Ring full (reader stalled): the tap drops the frame. All counted; the trim is logged on each
  100 ppm change (at most once a second) and summarised every 10 s.
- Mixing (Astro A50 style, attenuation only): earpiece = master × (chat × chat_level + game ×
  game_level) + master × (sidetone, centre tick). Balance -5..+5 (default 0 = both 100 %); each step
  toward game cuts chat by 20 % (-5: chat 0 %), toward chat cuts game the same way. Master
  0..100 % in 10 % steps, default 100 %. Q8 gains, never above 1; a gain change ramps over one
  8 ms block (no zipper click); every addition clamped. Chat is scaled after the loopback, so
  the 360 gets back what it sent, never the game or our gains. Settings are in memory only.
- One earpiece consumes the ring (xw.xex streams one headset).

### 8.6 Control and status

`XVGA` block (`voice.py status`; `voice.py game on|off|balance N|master N|sidetone N`):
enable, balance (signed), master, sidetone (xbdm-writable, clamped and written back if out of
range), the applied values and gains, state, error, handle, process type, tap calls and rate,
frames, capture errors, overflow, NaN frames, input peak, ring write/read/fill/average/target,
trim ppm and changes, starts, underruns, overruns, output peak, consumer pad and format.
`XVHS` block (version 3, voice_360.c): per pad mute, sidetone gain, guard factor and trips,
ear level, FIFO fill, gaps, skips, centre ticks; then (version 3, appended) the sidetone's
path and mode, the headset's last volume report and what we last sent it, its guard trips and
the software path's delay estimate (`voice.py status`, `voice.py sidetone mode|devmax|...`).

Share button (045e:0b12), the only control: tap < 1 s with no other button pressed meanwhile
= mic mute toggle (on release). While Share is held: D-pad Left/Right = balance (more game /
more chat), Up/Down = master ±10 %, LB/RB = sidetone -/+ 1 (0..10). Those presses are masked
from the XInput state titles see, each until it is released (a control already held when
Share went down stays the title's). The on-screen bar (hud_360.h) is published from the driver
thread on every change: visible while Share is held, and for 1.5 s with the mute flashed
after a mute tap.

Sidetone (voice_360.c): the own mic, level x master (not balanced), muted = none, through a
FIFO with no deliberate delay (primed with 10 ms: one 8 ms mic block + margin; gaps and stalls
fade over 2 ms). Feedback guard on the output RMS (50 ms windows): rising for 300 ms, one
window above -12 dBFS, or never below -24 dBFS for 2 s pulls the gain down 12 dB for 1 s, then
it recovers over ~1.4 s to 6 dB under the trip point until the level/master is changed.
Centre tick: when a balance step lands on 0, 15 ms of 2 kHz under a raised cosine, peak 1000
(-30.3 dBFS) x master. Offline-tested only (host build of voice_360.c).

#### 8.6.1 The headset's own sidetone (branch `sidetone`, Nov 23 2026)

Why the software sidetone "didn't work" (console read, Nov 23 2026, build 412aa18): the level
was 0 the whole session (XVGA sidetone 0/10, XVHS mixed samples 0, guard never tripped, mic
not muted). The log shows the owner on the panel pressing Up/Down and LB/RB (GIP buttons 0x1000 /
0x2000), the hs-controls gesture; the panel since WP1.3 ignores bumpers and only Share, Down,
Down, Right raises SIDETONE. Had it been on, it would have lagged by ~64 ms (the 8 ms mic
block, half a ~16.6 ms loop pass, the FIFO, the 40 ms of OUT blocks queued on Usbd): an echo,
not a monitor. And our start-up volume message set the headset's own sidetone to 0.

MS-GIPUSB 3.2.5.1.1 (Audio Control Volume Extended, both ways; xone's `gip_pkt_audio_volume`
calls the sidetone byte `unknown1`): command 0x08 system, 8 payload bytes: control type 0x03,
flags (0x01 capture muted, default muted; 0x02 render muted; 0x04 headset detected), speaker
volume, game/chat balance (0 all game .. 100 all chat; read-only on mono), mic volume (default
100), **sidetone volume (default 0)**, 2 reserved. Bit 7 of each field = writable by the host,
bits 6-0 = 0..100 %. A device that marks a field writable SHOULD answer a host request with the
values it applied; if it can't, it echoes the request and then sends its real values. (Xbox
One/Series consoles show a "Mic monitoring" slider for headsets in the pad's jack; per a
Microsoft Community answer, only when the attached headset offers it. Unverified on this pad.)

Implementation: `core/gip.c` keeps and logs every report (`gip: client N headset volume report
K: flags .., speaker W|ro N, ..., sidetone W|ro N`) and sends `gip_headset_set_volume`.
`voice_360.c` `xv_headset_volume` (called by `xw_plugin.c` every loop pass while the headset is
ready; a changed message at most every 50 ms) picks the path from `st_mode` (XVHS): 0 software,
1 the headset's own, 2 auto (default: the headset's own when its report marks SIDETONE
writable). The headset's sidetone = SIDETONE x 10 % of `dev_max` (100), not scaled by the
master volume (a MixAmp's monitor has its own knob), 0 when muted or at 0; the software
sidetone then ramps out. Its feedback guard estimates the ear level as the mic RMS (20 ms
windows, as it arrives) x the sidetone %: one window above -6 dBFS, or never under -24 dBFS for
2 s, sets a quarter for 1 s, then at most half until the level changes. Speaker and mic volume
are xbdm settings at 100 % (`devspeaker`, `devmic`): the gate, meter and mic level work on the
mic as the headset sends it. `dev_bit7` (default 0, as xone) sets bit 7 on the written fields
in case the headset wants it. Panel: Share + LB/RB = sidetone -/+ again (XWPN `bumpers`,
default 1), as well as the SIDETONE row. Acceptance: `accept/sidetone.py [--hands]`.

If the headset ignores it (WP1.6): the best a software sidetone can do is to mix the mic in
the IN completion DPC into the OUT block that goes out 2 frames later: ~8 ms (one IN block) +
2 ms margin + the headset's own buffering, so ~12-20 ms; from the driver thread (16.6 ms
passes) no better than ~30-40 ms. Not built.

Settings and diagnostics (`XWDG` block, version 2, `diag_360.h`; `xw.py diag [NAME VALUE]`;
docs/PLAN.md WP1.4). Compiled defaults, so nothing needs setting after a reboot: **mic level
50 %, noise gate low** (on the console, Nov 22 2026, these two stopped Halo 3's hot mic),
sidetone 0 (until WP1.5), test tone off, game buffer 45 ms. Mic level 10..100 % in 10 % steps
(Q15, 10 ms ramp; `xd_mic_gain_step` gives the panel the next accepted value). The gate and
the mic meter share one 20 ms RMS of the mic as it arrives (before the gain), computed for
every mic packet whether or not a title is capturing, muted or not: the gate opens at RMS 184
(-45 dBFS, low) or 583 (-35 dBFS, high), holds 200 ms, ramps 10 ms; the meter
(`xd_mic_vu`, 0..10) is that RMS after the gain, segment edges at -57 -51 -45 -39 -33 -28
-23 -18 -12 -6 dBFS, 0 after 100 ms without mic audio. `xd_gate_open` is false while muted.
The earpiece keeps 5 OUT blocks (40 ms) on Usbd, fixed (0 gaps measured with 5). Removed in
WP1.4: the clock-fix experiment (`ear_fb`, the variable-length packets it needed in
core/gip.c), the DPC refill, the `out_blocks` knob, the game tap / resampler dumps. Kept on
xbdm: `xw.py micdump` (raw mic), `xw.py dump` (the final earpiece PCM as a WAV), the buffer,
the tone, the OUT/IN counters and the headset's mic frame skips (`XWAU` `mic_skips`, now
version 3: `xw.py` refuses other versions instead of mis-reading them).

The driver loop's timing is in `XWDG` too: the pass time, split into the audio/voice work
(`work_us`) and the rest of the pass (`gap_us`: the 2 ms sleep, the panel/HUD/team polls,
the ports), a histogram of pass lengths, the timebase checked against the system clock, and an xbdm probe
(`xw.py diag probe sleep0..sleep20|tick`) that times one `xa_sleep_ms(n)`, or the system
clock's step, on the driver thread, once per pass for 16 passes. `accept/wp1_4.py` runs them
and prints a verdict on where the ~16.6 ms pass measured on the console goes. Offline, in
the 17559 dump: the clock interrupt (0x800771F8) adds 10000 x 100 ns to KeTimeStampBundle's
interrupt and system time (0x801B0EE0, what KeQuerySystemTime at 0x80071948 reads) and 1 to
its tick count, and timers are due against that interrupt time. Measured on the console
(Nov 23 2026, `accept/wp1_4.py`): the system time steps every ~5.4 ms, `xa_sleep_ms(1)` takes
14.9 ms and `xa_sleep_ms(2)` 15.9 ms, so the ~16.6 ms loop pass is the sleep (verdict SLEEP
GRANULARITY). 17559 exports no timer-resolution call (the SDK's xboxkrnl.def has KeSetTimer/Ex,
NtSetTimerEx, KeQueryInterruptTime and nothing like NtSetTimerResolution), and a 1 ms
relative KeDelayExecutionThread is what `xa_sleep_ms(1)` already is. An attempt to wake the
loop from the OUT completion instead (an event set on Usbd's DPC, gated `loop_wake`) froze
the console with it on, and was removed (Nov 23 2026): the loop stays at ~16 ms, which 5 OUT
blocks (40 ms) cover with no gaps. Anything that needs less latency than a loop pass (WP1.6's
late sidetone mix) has to run in the completion DPC itself.

### 8.7 Unverified, risks

- Nothing has run on the console. The MEC slots may be taken (Kinect uses at least one when
  connected; if both are, game audio stays off and `voice.py status` says "retrying"; with ours
  in place a later Kinect may find no slot, so its echo cancellation could fail).
- Channel order and planar layout come from Xenia; if wrong, channels land on the wrong side
  (audible, harmless). `in_peak` in the status shows whether anything arrives.
- What the hardware path does after the MEC call (e.g. whether the system volume or the
  master fade at 0x8012D158 apply before) only matters for level: we hear the mix as the TV
  would before the TV's own volume.
- The render loop's pacing wait was not traced; `tap_rate` should read 187-188.
- A title or XAM that calls `XAudioSetProcessFrameCallback` and modifies the mix in place
  changes what we copy (we copy after it), as it changes the TV output.

## Appendix: addresses used

Kernel: XVoiced exports above; lookup helpers 0x80102918 (0x1/0x4/0x5 → 0x80188050+idx·1440),
0x801065B0 (0x2 → 0x801896D8+idx·496), 0x80101C28 (0x9), 0x800FDA38 (0x7 → 0x801AA680);
wired headset driver AddDevice 0x801069F8, RemoveDevice 0x80106C08, status completion
0x80107328, audio completion 0x80107140 (ep0 0x80107318, ep1 0x80107320), transfer starter
0x80106EA8, packet cancel 0x801065C8, category-1 bind 0x801064A4, unbind 0x80106560; XAM
callback dispatchers 0x8012BB88 (Guide, +0x0C) and 0x8012BBD0 (bind, +0x10); Usbd
`UsbdQueueIsochTransfer` 0x800D82C0 → OHCI 0x800DC9A8 / EHCI 0x800DFA48, isoch completion
0x800DCCE0 → 0x800D5D20, async completion 0x800D5CC0, OHCI open endpoint 0x800DC310, ED init
0x800DC098, built-in class dispatch 0x800D62A0..0x800D6540, runtime-list walk 0x800D6178.

XAM: `XamUserGetDeviceContext` 0x816D8A58, bind-with-connect 0x816D8CD8, unbind 0x816D8F50,
`XamUserBindDeviceCallback` 0x816D9060, per-category callbacks 0x81A82FF8 (cat 1 = 0x816FC228),
voice table 0x81AACA18 (+16 title ctx table, +240 system), `XamVoiceCreate` 0x816FC098,
`XamVoiceHeadsetPresent` 0x816FB418, `XamVoiceSubmitPacket` 0x816FB498, `XamVoiceClose`
0x816FC1C8, `XamVoiceGetBatteryStatus` 0x816FB3C8, XEX export table at 0x81A73D34.

Reference files: `core/gip.c:845-876` (client-id drop), `xbox360/xinput_360.c` (bind with
category 0), xone `bus/protocol.c` (audio control 0x08 sub 0x00-0x03, samples 0x60),
`driver/headset.c`, `transport/wired.c`.
