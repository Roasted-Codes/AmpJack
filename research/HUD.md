# The on-screen headset panel (HUD)

While the controls say so (Share held: the panel; a mute tap: a toast), xw.xex draws a panel
into whatever is on screen. Code: `xbox360/hud_360.c` (hook, swap records, fetch decode,
tiling, blit, flush, status; WP1.1) and `xbox360/hud_panel.c` (what the panel looks like:
`hp_build`, `hp_sentinels`, the font and the palette; WP1.2). Contracts: `hud_360.h` v2
(`hud_publish`, `hud_init`, `hud_poll`, `hud_quiesce`, `hud_resume`; docs/PLAN.md 1.0.1) and
`hud_panel.h` (1.0.2). Dev tool: `xbox360/hud.py`. Console bring-up: `xbox360/accept/wp1_1.py`
(section 4).

Each finding is marked **verified** (read in the 17559 dumps in `research/dumps/`, or checked
offline) or **inferred** (from Xenia's sources or reasoning, not yet seen on the console).

## 1. Findings (kernel 17559)

### VdSwap

- **Ordinal 603 (0x25B)**, at **0x800C8B90**. *Verified*: the kernel's export table in the dump
  gives 603 → 0x800C8B90. Xenia's `xboxkrnl_table.inc` lists 0x25B as `VdSwap`. The same parser
  gives XVoicedHeadsetPresent (481) at 0x80101C40, which matches `voice_360.c`.
- **Prototype** (*verified* from the disassembly; Xenia's names in brackets):
  `void VdSwap(u32 *cmd /*buffer_ptr*/, const u32 fetch[6] /*fetch_ptr*/, u32 *fence_addr /*unk2*/,
  const u32 *info /*unk3*/, u32 fence_value /*unk4*/, u32 *out_frontbuffer, u32 *out_format,
  u32 *out_colorspace, u32 *out_width /*9th, caller frame 0x54*/, u32 *out_height /*10th, 0x5C*/)`.
  - `cmd` points to 64 words of command space that D3D reserved. The kernel fills it and pads the
    rest with a type-3 NOP.
  - `info[0]` and `info[1]` are an indirect buffer (D3D's own swap commands), emitted as
    `0xC0013F00` (type 3, `PM4_INDIRECT_BUFFER` 0x3F) when `info[0] != 0`. The opcode meaning is
    *inferred* from Xenia's PM4 list.
  - If `info[0] != 0`, the kernel copies `fetch` (6 words), sets its clamp and filter fields,
    converts its base address to physical, and writes it to fetch constant 15 (type-0 packet
    `0x0005485A`, registers 0x485A..0x485F). This is the constant the display uses for the front
    buffer. In 17559, `info[2..6]` are copied out to the five out-pointers: frontbuffer,
    format, colour space, width, height. In Xenia's version these are inputs.
  - **The fence:** it always ends with `0xC0025800, 3, phys(fence_addr) | 2, fence_value`
    (0x800C8CA4..0x800C8CE8). The packet is type 3, opcode 0x58, which is `EVENT_WRITE_SHD` in
    Xenia (*inferred*): when the GPU gets there, it writes `fence_value` to `fence_addr`, with
    the 8in32 swap (the `| 2`). The packet layout is *verified*. That it makes a good "this swap
    has been processed" signal is *inferred*. `hud.py status` shows `fence_same` to check it on
    hardware.
  - Virtual to physical (*verified*, 0x800C8C04..0x800C8C2C): `(va & 0x1FFFFFFF) + 0x1000` if
    `va >= 0xE0000000`, else `+ 0`. So the fetch's base is a CPU virtual address in the
    physical-memory windows 0xA0000000..0xFFFFFFFF.
- **Prologue** `7d8802a6 48044021 9421ff40 3d608017` (mflr r12; bl __savegprlr; stwu; lis r11).
  *Verified.* `detour.c` could not hook this before: its scratch-register check counted the `0`
  in `lis r11` (addis rD, **0**, imm) as a use of r0. It now treats rA = 0 of addi/addis as the
  literal it is. The trampoline uses r0, which is free there.

### The front buffer (from the fetch constant; layout from Xenia's `xenos.h`)

| word | bits | field |
|---|---|---|
| 0 | 0-1 | type (2 = texture) |
| 0 | 22-30 | pitch, in 32-texel units |
| 0 | 31 | tiled |
| 1 | 0-5 | format: 6 `8_8_8_8`, 7 `2_10_10_10`, 50/54 their `AS_16_16_16_16` variants, 32 `16_16_16_16_FLOAT` |
| 1 | 6-7 | endian: 0 none, 1 8in16, 2 8in32, 3 16in32 |
| 1 | 12-31 | base address >> 12 |
| 2 | 0-12, 13-25 | width - 1, height - 1 |
| 3 | 1-12 | swizzle (3 bits per output channel X Y Z W: source component 0-3, 4 = 0, 5 = 1) |

The field positions are *inferred* from Xenia. The kernel's own edits *verify* the positions of
`base` (it masks word 1 with 0xFFFFF000), the clamp field in word 0 and the filter field in
word 3. Xenia's VdSwap asserts the format is `8_8_8_8` or `2_10_10_10_AS_16_16_16_16`. The
display hardware scans out only those, so 16F front buffers are skipped.

Not yet observed on hardware: the actual fetch constants for a real title, Aurora and the
dashboard. `hud.py status` prints the last one, raw and decoded. Expected values: tiled, 8in32,
`8_8_8_8` or `2_10_10_10`, 1280x720.

### Tiling

The 2D tiled address (32 bpp) is Xenia's `texture_address::Tiled2D`, which is the same
function as the XDK's `XGAddress2DTiledOffset`. *Verified offline* in three ways:
1. Xenia's formula and the XDK formula agree on every texel of 160x96 at 4 and 8 bytes per
   texel.
2. At 1280x720 every texel maps to a distinct address (a bijection).
3. `hud_360.c`'s implementation, de-tiled by an independent Python copy of the XDK formula,
   gives a clean picture: 8_8_8_8 and 2_10_10_10, all four endian modes, tiled and linear,
   640x480, 1280x720 and 1920x1080, and the opaque-repaint path.

Properties used:
- A 32x32 macro tile of 4-byte texels is exactly one 4 KB page. The page check calls
  `MmIsAddressValid` once per macro tile.
- A 128-byte cache line holds 16x2 texels: offset bits 0-3 are x0-1, bit 4 is y0, bit 5 is
  x2, bit 6 is x3. The flush walks 16x2 blocks.

### Why not draw inside the VdSwap hook

VdSwap only queues GPU commands. The title queued its resolve (EDRAM → front buffer) just
before VdSwap, and the GPU usually runs a frame behind the CPU. So when VdSwap runs, the
resolve has usually not happened yet, and pixels written then would be overwritten
(*inferred*, from how the PM4 ring works). That would mean flicker, or no bar at all in
GPU-bound games. The fence written right after the swap tells us when the frame is really in
memory.

Alternatives considered:
- Drawing from the GPU interrupt. *Verified*: D3D's callback is stored at 0x801863D8/DC by
  VdSetGraphicsInterruptCallback (469, 0x800D0340) and called with source 0 at 0x800D0414 and
  source 1 at 0x800D0570. Rejected: it runs in interrupt context, and source 1 isn't only for
  swaps.
- GPU-side drawing with PM4 packets. Rejected: a bad packet hangs the GPU in every game.

## 2. Design

- **Hook (title render thread):** `XhSwapHook` is an asm stub. It saves r3-r10 and the 64-bit
  r14-r31, then calls `XhSwapC`. `XhSwapC` records the call into an 8-slot ring: the fetch
  constant, the `info` words, the fence address and value, and the title handle. It reads only
  what VdSwap itself reads. It has no locks, uses one `lwarx/stwcx` ticket, and has a
  per-slot sequence number. The stub then restores every register and jumps to the trampoline
  with the caller's LR and stack, so VdSwap sees its stack arguments 9 and 10 unchanged.
- **Draw (driver thread, `hud_poll`, once per pass of its loop):** the loop sleeps 2 ms, but
  on the console a pass takes ~16.6 ms (section 6), so `hud_poll` runs ~60 times a second.
  While the panel is visible at stage 3, it takes the newest recorded swap whose fence the GPU
  has written and draws into that buffer once. Before drawing it checks:
  - the swap is from the same title;
  - the swap is under 250 ms old;
  - the format is known;
  - the panel fits the title-safe area;
  - every touched page is mapped;
  - every offset is below the buffer's size.
  After drawing it flushes the touched lines with `dcbf` and runs `sync`. That swap's frame is
  displayed from the next vblank until the next swap.
- **Published state:** `hud_publish` writes under a seqlock. The driver thread reads it.
- **Panel:** `hp_build` (hud_panel.c) fills a canvas of up to 240 x 104 units of palette
  indices (`HP_CLEAR` = transparent); its height depends on the rows shown (82 units for
  Phase 1's 6 rows; the toast and NO HEADSET are 120 x 15). It is rebuilt only when the
  published state changes, and drawn at an integer scale: 2 at 720p, 3 at 1080p, 1 at 480
  lines, stepped down until it fits the title-safe area (the middle 80 % each way). It sits at
  the bottom centre, its bottom edge at 90 % of the height. At 720p the full panel is
  480 x 164 px; at 1080p 720 x 246.
  - Colours: `hp_palette` (hud_panel.c). hud_360.c turns them into texel words for the
    buffer's format, endian mode and channel order.
  - **Opaque by default** (`keep` 0): the background is written, never read. The only reads
    of the front buffer are the 4 sentinel texels. `keep` > 0 blends the background toward
    `HP_BG` with integer math (a read per background pixel), as hs-hud did.
- **Sentinels:** `hp_sentinels` names 4 text units (the first lit pixels of the anchor text).
  Their top-left pixels carry the unit's colour with the low bit of blue flipped, plus red's
  for sentinels 1 and 3 and green's for 2 and 3. If all four are already in the buffer, the
  frame was swapped again without a new resolve: an opaque panel is then not drawn again
  (`intact`), a blended one is repainted opaque (`repaints`). The last draw's sentinel screen
  pixels and colours are in the status block (`sent_*`), so `hud.py shot --find` and the
  acceptance script check those exact pixels, at any scale.
- **Blit:** the tiled address splits into a column part and a row part. Tiled2D puts x's
  bits at offset bits 2, 3, 5, 6 and (x >> 5) << 12, y's at 4, 8-11 and the macro-tile row,
  and bit 7 = x4 ^ y3, so `offset = col + row - 2 x (col & row & 0x80)`. Both parts are
  tabled once per geometry, and a pixel costs a table load, three ALU ops and a store.
  *Verified offline* against `tiled_offset` for every texel of pitches 640..2048 up to 1100
  rows, and by a host test that draws the panel (8_8_8_8 / 2_10_10_10, all endian modes,
  tiled and linear, 640x480 to 1920x1080, keep 0 and 80) and checks every word of the buffer;
  shot.py finds the sentinels in that frame, blind and at the reported positions.
- **Channels:** R, G and B lane positions come from the endian mode and the fetch swizzle,
  so 8888 and 2_10_10_10 work in any byte order. Alpha bits are kept when blending.
- **Cost:** while the panel isn't visible: the hook's few dozen stores per frame, and
  `hud_poll` copying 1-2 records per pass. While it is visible: one store per panel pixel
  (78,720 at 720p, 177,120 at 1080p) per drawn frame, no reads, on the driver thread.
  `hud.py status` shows the time per draw (`d_ticks`, timebase at 50 MHz, a 4-bucket
  histogram) and `hud_poll`'s own time (`poll_us`).

## 3. Stages, gates and dev controls

- `enable` (XH36 word 2) is the **stage**, **0 at boot**, written over xbdm with
  `hud.py stage N` (docs/PLAN.md WP1.1):
  - 0: nothing. VdSwap is not looked at. Once hooked, the hook only passes through.
  - 1: check only. Resolve ordinal 603 and compare 23 code words (prologue, argument moves,
    info test, fetch copy, fence packet) with the 17559 dump: `sig_ok` 1, or 2 with the first
    differing address in `error`. Nothing is patched. (A hot-reload slot build whose
    VdSwap prologue is an earlier build's hook checks the other 19 words.)
  - 2: hook VdSwap (only if `sig_ok` is 1) and record swaps. Never draw.
  - 3: drawing allowed (when the controls, or `hud.py show`, make the panel visible).
  Lowering the stage never unpatches. Each stage change and each new (title, format) pair
  gets one log line (`hud: ...`).
- Hot reload (`hud_360.h`, docs/HOT-RELOAD.md): `hud_quiesce` makes the hook record nothing
  and only return the trampoline, and `hud_poll` do nothing (`quiesced` 1); `hud_resume`
  undoes it. The hook is never unpatched; the next build adopts its slot.
- xbdm block "XH36" (`g_xh`, version 2; version 1's 62 words are a prefix, so `xw.py tee`'s
  offsets hold), with `hud.py` (`XBOX=10.0.0.13`):
  - `hud.py status`: stage, signature, hook, swaps and draws, rates (`swaps_per_s`,
    `draws_per_s`, `cover_pct` = draws / swaps while the panel is up, and the cumulative
    `vis_swaps` / `vis_draws`), timing (`d_ticks` and its histogram, `fence_late_us` = fence
    seen to draw start, `swap_late_us` = VdSwap called to draw start, `poll_us`), skip
    reasons, the last fetch constant (raw and decoded), the last 4 (title, format) pairs
    (`tt_*`), and the last draw (geometry, sentinels).
  - `hud.py show [visible=1|2 muted=1 cursor=2 rows=0x3f balance=-3 ...]`: **force visible**
    with this test state (`force` = 1). The keys are struct hud_state v2's field names
    (`test_*` controls in XH36); unnamed keys keep their last value. Needs stage 3.
  - `hud.py hide` (`force` = 0, the controls decide); `hud.py never` (`force` = 2).
  - `hud.py set mode N`: 0 draws once the fence passes (default). 1 draws when the swap is first
    seen (a fallback if the fence is unusable; it can flicker). 2 draws the previous swap's
    buffer.
  - `hud.py set swap_rb 1` if the green comes out red or blue. `hud.py set keep N`: blended
    background (reads the buffer).
  - `hud.py set d_ticks_max 0` (and `fence_late_max_us`, `swap_late_max_us`, `poll_max_us`):
    measure a maximum from now.
  - `hud.py shot FILE.png [--find]` (shot.py, WP0.2): the front buffer as a PNG; `--find`
    checks the sentinels at the positions the last draw reports.

## 4. Bring-up on the console (WP1.1, H0-H8)

`xbox360/accept/wp1_1.py STEP`, one step per run, with `xw.py tee` going, about 30 s apart:
h0 (boot, read-only), h1 (stage 1, signature), h2 (stage 2 at idle Aurora: hooked, 60 s
alive, ~60 swaps/s, nothing drawn), h3 (format, tiling, size, fence behaviour), h4 (stage 3 +
show: drawn, sentinels in a screenshot), h5 (20 screenshots, `--modes` compares modes 0/1/2),
h6 (Halo 3 menu and match: h3-h5 plus cover >= 95 % and draw time < 1 ms), h7 (panel up 60 s
in a match: `out_starved` +0, `mic_pps` 1000, `out_frames_missed` +0), h8 (title switches with
the panel forced on), off. The script's docstring lists what each checks and what a failure
means.

## 5. What could go wrong on hardware

- **Patching VdSwap while a render thread is inside its first four instructions.** The
  detour parks new callers on `b .`, but a thread already past the first word would run a
  half-written prologue. The window is a few ns per frame. It is unlikely but not impossible.
- **The fence:** its meaning is *inferred*. If D3D reuses the location or the value isn't
  monotonic, mode 0 draws too early (flicker) or never. `fence_same`, `fence_waits` and
  `mode` exist for this.
- **Cost:** the panel is opaque by default, so the only front-buffer reads are the 4
  sentinels. With `keep` > 0, if the front buffer is write-combined or uncached, each
  background read is an uncached load: a few ms per frame on the driver thread (hardware
  thread 2, shared with Usbd's DPCs). If H6/H7 show the stores alone are too much, the
  pre-specified fallback is a draw thread (docs/PLAN.md WP1.1); it moves the cost, not the
  latency (section 6).
- **Single-front-buffer titles:** a title that resolves into the buffer being displayed tears
  the bar the same way it tears its own picture.
- **The title exits while the bar is up:** the title is checked before the fence read and
  again before drawing, and every page is checked as mapped. A title unmapping its memory in
  the microseconds between the check and the writes would fault the driver thread.
- **Unknown formats or swizzles** are skipped and counted, not guessed. A swizzle that the
  display hardware ignores would only put the accent colour in the wrong channel (`swap_rb`).

## 6. Timing on the console (read over xbdm, Nov 22 2026)

- The driver loop's pass (`XW36 loop_us`) is **16,623-16,625 us**, steady, although the loop
  sleeps 2 ms; ~280 ms at boot. The XH36 heartbeat (one per `hud_poll`) grows ~60.2 per
  second, and `now_ms` (KeQuerySystemTime) moves in 16-17 ms steps. So
  `KeDelayExecutionThread` wakes on the kernel's clock tick, about 16.6 ms here. *Verified*
  (read-only, the WP0.1 build at Aurora).
- Consequence for the panel: the driver thread sees a passed fence 0-16.6 ms after the GPU
  wrote it, and draws then (`swap_late_us`, `fence_late_us` measure it with the timebase).
  A 60 fps frame is on screen for 16.7 ms and the panel's rows are scanned out ~14 ms after
  vblank, so a late draw can miss that frame's scan-out at the panel's lines (seen as
  flicker), while xbdm screenshots may still show it (the buffer holds it). A 30 fps title
  (Halo 3) shows each frame for two refreshes, so the second refresh carries the panel. A
  draw thread that also sleeps wakes on the same tick: it would not shorten this.
  *Inferred*; H5 and H6 measure it (screenshots, `cover_pct`, `swap_late_us`), and the the owner
  checks at H4/H6 judge it by eye.
- `xa_now_ms` has the same 16 ms resolution: every HUD timing uses the timebase (`mftb`,
  50 MHz) instead; `STALE_MS` (250) is coarse enough.

## 7. Most swaps carry no picture (console, Nov 22 2026, WP1.1 H2/H3)

- With the hook in (306a69a, stage 2), `swaps` grew at the title's frame rate (60/s at
  Aurora, 30/s in a title rendering 1152x640, i.e. Halo 3) but almost every call had
  `info[0]` = 0: 558 of 9263 swaps carried a fetch constant, and none over 10 s of play.
  The kernel agrees: VdSwap clears the word at 0x80171100 on entry and sets it to 1 only on
  the picture path (0x800C8BBC / 0x800C8CA0); it read 0 every time. *Verified.* So the
  hook and its stub were right; VdSwap only queues the fence on those calls, and the buffer
  on screen stays the one the last picture swap set (fetch constant 15). The pictures that
  were seen: 640x480 8_8_8_8 and 1152x640 2_10_10_10_AS_16_16_16_16, tiled.
- Consequence: a title resolves each frame into the buffer that is already on screen and
  calls VdSwap for the fence. hud_360.c now takes the buffer for every swap from, in order:
  the swap's own fetch constant; the 6 words its r4 points to (D3D passes the pointer on
  every call; the driver thread reads it with MmIsAddressValid, the hook never does) if
  they decode as a front buffer; the last picture swap of the same title. The `src` control
  restricts that (1: own or last picture; 2: own or r4); `src_n[]` counts swaps per source,
  and `sw_ptr[]` / `sw_out[]` show r4, r8-r10 and what r8-r10 point to, for the next look.
  *Inferred*, to be confirmed at H3/H4: that r4 holds the current front buffer on
  picture-less calls.
- `swaps_per_s` counted only picture swaps (0 here); it counts every swap now.
- With the buffer reused, a frame can be swapped again after the canvas changed while our
  sentinels are still in it: the panel is now skipped as intact only if the same canvas
  generation was drawn into the same buffer, else drawn again.
- XH36 is version 3 (147 words; version 2's 127 are a prefix).

## 8. Where the picture is, and why a draw took 16.7 ms (third console run, Nov 22 2026)

- **xbdm's screenshot** (handler at 0x9100D5D0 in xbdm.xex, read over xbdm): it waits for
  the GPU (RBBM_STATUS, 0x7FC81740 bit 31) with the title stopped, reads the scan-out
  surface's physical address from **D1GRPH_PRIMARY_SURFACE_ADDRESS (0x7FC86110)**, maps it
  through the first valid of +0xC0000000, +0xA0000000, +0xE0000000 - 0x1000, takes the size
  from VdGetCurrentDisplayInformation (ordinal 442; width/height at +0/+2, display size at
  +0x48/+0x4A) and the format from D1GRPH_CONTROL (0x7FC86104) bits 0-2 (0: 0x18280186,
  8_8_8_8 8in32 tiled; else 0x182801B6). The kernel helper at 0x800B93F0 does the same
  mapping. The kernel writes 0x6110 itself only in the GPU interrupt after a picture swap
  (0x800D0100, flag 0x80171100); with picture-less swaps the title's D3D flips it.
  *Verified* (disassembly). xbdm's getmem refuses MMIO, so the register couldn't be read
  from the Mac.
- **The screenshot is byte-identical to the buffer `*r8` named** (fe6a0000 at the time;
  four 256-byte samples across the frame), so D3D's front buffers are what is scanned out.
  *Verified.*
- **The bug:** r4 (7004f7c0) and r8 (7004f7ac) point into the render thread's stack (D3D
  builds the fetch constant in its frame for each call). c65287a read them later from the
  driver thread, by which time they held another call's data: the panel went into whichever
  buffer that was (often the one not on screen; `intact` 276 = our panel still sitting in a
  buffer that wasn't being shown or re-rendered). The hook now copies the 6 fetch words
  and `*r8`/`*r9`/`*r10` at call time (only mapped, aligned pointers). `ptr_agree` /
  `ptr_differ` count whether `*r8` equals the copied fetch's base; `src 3` draws into
  `*r8`'s buffer with the fetch's layout if they ever differ.
- **16.7 ms per draw:** 78,720 stores in 16.7 ms is ~210 ns a store: the front buffer is
  not cacheable (0xE0000000 window, 4 KB pages, D3D's write-combined GPU memory), and the
  row-order blit wrote each 128-byte line in 16-byte pieces at different times. The tiled
  blit now writes each line whole and in ascending order (`order` 0; 1 = the old row
  order), and the stores and the flush are timed separately (`d_blit_us`, `d_flush_us`;
  `noflush` 1 skips the dcbf). *Inferred*; the next run's `d_blit_us` says.
- XH36 version 4 (153 words).

## 9. Fourth run (H4 at src 0/1/3: drawn, never on screen; 33-49 ms a draw)

- `*r8` equals the r4 fetch's base in every swap (3999 of 3999), so src 3 adds nothing.
- The draw times are whole multiples of the ~16.7 ms tick (16.7, 33.4, 49.1 ms). Two
  readings: the stores are slow (~200-600 ns each: the 0xE0000000 mapping is not cached),
  or the driver thread loses hardware thread 2 for whole ticks in the middle of the blit
  (a title thread at higher priority). Either way a draw finishes 1-3 frames after the
  frame it was meant for, while the title renders into the same two buffers every frame,
  so the panel is overwritten before it is shown. Version 5 tells the two apart:
  `d_gap_max_us` / `d_gaps` (the longest pause between two 128-byte lines; a pause of
  ~16.7 ms = preempted), and tries faster stores: 8-byte stores (`wide` 1, default) and
  the same physical memory through another mapping (`alias` 1 E / 2 A / 3 C window,
  checked with MmGetPhysicalAddress).
- Read-only check after a reboot (stage 0): an xbdm screenshot differed from both
  fe308000 and fe6a0000 by a few words per 4 KB page (Aurora's background moves between
  the screenshot and the reads), so the screenshot alone can't say which buffer scans out.
  Version 5 reads the display's D1GRPH_PRIMARY_SURFACE_ADDRESS (0x7FC86110) itself, as the
  kernel (0x800B9408) and xbdm do, and counts whether each swap's buffer is physically that
  surface (`scan_agree` / `scan_differ`); `src 4` draws into the scan-out surface as xbdm
  maps it.
- If the pauses are the cause, the fix is the pre-specified draw thread on another hardware
  thread (docs/PLAN.md WP1.1 fallback), not faster stores.

## 10. Fifth run: descheduling, and the draw thread

- 184b488 at Aurora: the swaps' buffer is the scan-out surface (register 0x1E309000 ->
  0xDE309000 as xbdm maps it; 3768 of 4016 swaps physically equal, 0 differ). A draw took
  ~17 ms with one pause of ~15.6 ms between two lines and 27-62 pauses over 1 ms: the
  driver thread loses hardware thread 2 for a whole tick in the middle of a draw, and the
  stores themselves are fast. Through the C window (alias 3) the same; the A window isn't
  mapped. So the panel lands a frame or more late and the title overwrites it. *Verified.*
- Version 6 adds the plan's fallback, behind `thread` (default 0): a system thread of its
  own (`XhDrawThread`, NV64 thunk in hud_360.c) on hardware thread `thr_hw` (default 3;
  0, 1, 3 or 5), priority `thr_prio` (default 9), created at stage 3 when `thread` is 1.
  The VdSwap hook wakes it with KeSetEvent at every swap (a 16-byte KEVENT laid out as the
  kernel's, never the SDK's 4-byte type); it then ingests, picks the newest swap whose fence
  has passed and draws at once. `spin_us` (default 0, max 8000) makes it watch the newest
  swap's fence after a wake that found none ready, at low SMT priority. Exactly one of the
  two threads does the drawing side at a time (each sets its own busy flag, syncs, then
  checks the other's and the control). `hud_quiesce` waits for the thread to finish its
  pass; afterwards it stays parked in its wait (100 ms timeouts) and the image stays loaded.

## 11. Sixth run: on screen, but half a panel

- d235b3d, cold boot, `thread` 1 on hardware thread 3: 4 of 4 sentinels, but only the
  bottom half of the panel in the screenshot; a draw took ~33 ms with ~15.6 ms pauses, so
  the title rendered over the top rows while the thread was off the CPU. `thr_prio 15` left
  it at 9.
- Why 15 didn't take: KeSetBasePriorityThread (ordinal 153, 0x80070A38) sets the base to the
  process base (KTHREAD +0xC9) + the increment, clamped to [+0xC8, +0xCA]; +0xCA is 9 for a
  system thread. *Verified* (disassembly). Version 7 raises the draw thread's own ceiling
  (+0xCA) to the asked priority first when it is above 9 (only then: the default stays 9),
  and reports base and ceiling (`thr_prio_base`, `thr_prio_max`).
- `thr_hw` now moves the running thread (KeSetAffinityThread, ordinal 151) at its next wake.
- A pause longer than `retry_us` (default 2 ms) between two lines restarts the draw from the
  top, up to 3 times (`d_restarts`, `d_tries`), so a finished draw is one uninterrupted pass.
- `bg`: 1 opaque (default), 0 no background (12,424 of 78,720 pixels at 720p), 2 background
  only within one unit of text and shapes (27,776 pixels). Over a buffer that still has an
  older panel of ours (sentinels found), the full opaque canvas is written so no stale text
  is left.
- `d_paused_us` (time lost in pauses over 200 us) and `d_work_us` (the stores themselves)
  split `d_blit_us`; `d_ink_px` is the pixels written.

## 12. Seventh run: whole and fast, but in 15-30 % of screenshots

- 6672dbb, draw thread on hw 1 at priority 15: no pauses, 1.6 ms for 78,704 pixels, 1
  attempt; sentinels in 3 of 20 screenshots (mode 0 6/20, 1 3/20, 2 0/20). VdSwap -> draw
  33.3 ms; fence seen -> draw 10 us; 94-100 % of swaps drawn.
- What the counters say (`hud_status.txt` of that run): two front buffers alternate
  (fe308000, fe6a0000); at the moment a swap is taken in (microseconds after VdSwap) its r4
  buffer **is the one being scanned out** in 13061 of 13252 swaps. Its fence passes ~2
  frames later. So D3D passes in r4 the buffer currently shown (the kernel ignores r4 on the
  picture-less path); by its fence, that buffer has been displayed and the title has
  probably rendered its next frame into it. The draw thread, woken only by VdSwap calls,
  draws whichever swap's fence passed, at a phase that varies against vblank: the panel is
  sometimes in the buffer on screen for the rest of a refresh, often not. *Inferred* from the
  counters; version 8's trace shows the timeline.
- Screenshots are also a pessimistic probe: xbdm stops the title, waits for the GPU (100 ms
  sleeps), then reads the scan-out. With no more VdSwap calls the draw thread (100 ms
  timeout) was usually not woken in that window, so the last frame never got the panel.
- Version 8:
  - `ensure` (default 1): every pass, read the scan-out register; if it is one of the
    title's last four front buffers (physically) and lacks this canvas (4 sentinel reads),
    draw there now, with that buffer's own layout. Counters `ens_checks`, `ens_draws`,
    `ens_skips` (scan-out not a known front buffer: e.g. the Guide; nothing drawn).
  - `idle_ms` (default 20): the draw thread's wait when no swap wakes it, so a stopped or
    static title gets its scan-out buffer drawn within about a tick.
  - "already drawn" is now per buffer (the canvas generation last drawn into each of the
    last four physical buffers), so alternating buffers aren't redrawn needlessly.
  - `hud.py trace [N]`: the next N (<= 64) events with timebase stamps: wakes (with the
    scan-out), swaps taken in (buffer), fences seen, draws and their ends (scan-out after),
    scan-out changes. Buffers print as letters.
- Not built: timing draws to the flip with the GPU interrupt. D3D owns the one interrupt
  callback (0x801863D8/DC, set through VdSetGraphicsInterruptCallback); chaining it means
  rewriting a kernel global the title re-sets at every launch, and the callback runs in the
  GPU interrupt's DPC. Possible later, if `ensure` + the trace show the flip phase matters.
- Not built: drawing with GPU commands appended at VdSwap (after the title's resolve). It
  would be frame-exact, but needs a hand-written PM4 stream (render state save/restore,
  Xenos shader microcode or a memexport/copy), and one malformed packet hangs the GPU in
  every title: a hard freeze with no log. High effort, high risk; not worth it while the
  CPU path still has untried timing.

## 13. Eighth run: the owner sees it; flicker; video modes

- On the TV the panel is readable and follows Share + D-pad live, but flickers at times.
  xbdm screenshots showed it in 0 of 20, so they don't measure this (section 12); the
  trace does.
- Trace (title 30000ea8, 1152x640 2_10_10_10_AS_16_16_16_16 tiled, 30 swaps/s): one
  scan-out buffer A for every swap; the fence is seen 19-21 ms after the swap, and the draw
  follows 15 us later (1.6 ms). The title resolves its next frame into A, the buffer on
  screen, so the panel is gone from that resolve until our next draw. The fence was seen
  only at the draw thread's next wake (20 ms idle timeout), so that gap could be most of a
  frame. The 20 ms timeouts came back after 19-21 ms: the draw thread's waits are not tied
  to the driver loop's 16.6 ms tick. *Verified* (trace).
- Version 9:
  - `idle_ms` default 2: the draw thread wakes every ~2 ms; each pass `ensure` checks the
    scan-out (4 sentinels plus the lowest text unit, against a resolve that has only
    reached the bottom rows) and redraws at once if the panel is gone. The page-mapped
    check is repeated for the same buffer only every 250 ms, so a pass that finds the panel
    costs a register read and 5 texel reads.
  - Presence: `pres_yes` / `pres_no` (samples), `pres_pct` (last second), `gap_last_us` /
    `gap_max_us` (last sample with the panel -> redrawn). `wp1_1.py pres` runs it for 10 s.
  - Trace: "gone" events (panel missing, redrawn, how long since last seen); wakes only
    with `hud.py trace N --wakes`.
- Video modes (*inferred* from how it works; not yet tried beyond Aurora's 1280x720 and
  this 1152x640 title): the panel is written into the title's front buffer at the title's
  render size, before the console's scaler produces the output mode. So 480p / 720p /
  1080i / 1080p and HDMI / VGA / component don't matter: the panel is scaled with the
  picture. The scale comes from the buffer's height (1 below 540 lines, 2 for 540-899,
  3 from 900), stepped down to fit the middle 80 % each way. A 4:3 title (640x480) gets a
  4:3-shaped panel, stretched or pillarboxed exactly as the game is. Formats drawn:
  8_8_8_8 and 2_10_10_10 with their AS_16_16_16_16 variants, any endian mode, tiled or
  linear, 320-2048 x 240-2048, pitch up to 4096 texels. Skipped and counted, never
  guessed: other formats (16_16_16_16_FLOAT, 64-bit), a scan-out that isn't one of the
  title's last four front buffers (the Guide and other system screens: `ens_skips`), and a
  canvas that doesn't fit. Colours in 2_10_10_10 are the 8-bit palette widened; titles
  whose front buffer is gamma or HDR-encoded differently show the panel slightly off-tint.
