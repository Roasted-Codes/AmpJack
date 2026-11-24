# PLAN: MixAmp replacement on the Xbox 360 (xw.xex)

Repo `/Users/owner/Documents/code/Xbox360-GIP-Wired`, branch `headset-audio` (HEAD 5eabd0a). This is a plan only: nothing was edited and nothing was run on the console.

Four items are flagged before the plan itself, because they change the order of work:

1. **The overlay cannot hook VdSwap from the current HEAD.** `hs-hud` (4266a69) changed `names_reg()` in `xbox360/detour.c` so `addi/addis` with rA = 0 no longer count as using r0. HEAD's `detour.c` does not have that case (the default arm still returns `d == ur || a == ur`). Without it, `detour_install` on VdSwap's prologue (`mflr r12; bl; stwu; lis r11`) fails with "no free scratch register". `hs-hud` also branches from c15d8f8, before hot reload, so its `hud_360.c` has no `hud_quiesce`/`hud_resume`. WP0.1 fixes both.
2. **Sidetone will be late, by construction.** The mic arrives in 8 ms blocks, passes a FIFO primed to 10 ms, and then sits behind 5 OUT blocks (40 ms) already on Usbd. Expect roughly 55-60 ms from mouth to ear; a MixAmp is 0 ms. This is computed from the code, not measured. Phase 1 measures it and has a fallback (WP1.6).
3. **Hot reload skips the settle wait that boot uses.** At boot the plugin waits for the title switch to settle for 8 s before touching Usbd or XAM (`wait_for_title_switch`, skipped when `g_pred` is set). On a reload, `xw.py` writes `cmd_takeover` as soon as the module appears, which is while the loader title is launching Aurora. That is my top suspect for both freezes (Phase 3).
4. **HaloLAN has no source on this Mac.** I searched `/Users/owner/Documents/Code` and found nothing. What it hooks and which network calls it uses must be read from the console (WP3.0, WP4.0). If it detours any of our 7 hooked functions, hook adoption at reload falls back to a fresh patch of live code.

---

## 0. Ground rules for every package

- Kernel rules (`docs/INTERNALS.md`): no FP, no .bss, an NV64 thunk for every kernel-called entry, Usbd only on its DPC, XAM UI/bind calls only on the XAM thread. `check_xex.py` must pass.
- Every new feature has an xbdm-writable gate that defaults to off until its acceptance test has passed on the console. Then the default flips in a separate one-line commit.
- Status blocks are all-u32, magic-tagged, with a `_Static_assert` on size and a mirrored field list in the Python tool. Append only.
- One console (10.0.0.13) is the scarce resource. The coordinator owns it. Each package delivers an acceptance script `xbox360/accept/<wp>.py` that prints `PASS`/`FAIL` lines and exits non-zero on failure. One file per package, so there are no merge conflicts.
- the owner never looks at the Mac. Anything that needs eyes or ears is listed as a "the owner check" with one sentence telling him what to do and what he should perceive.
- Every console session runs `xw.py tee` (WP0.2) so a log survives a freeze.

### File ownership (who may edit what)

| File | Owner package |
|---|---|
| `xbox360/hud_360.c` (hook, swap records, blit, status), `hud.py` | WP1.1 |
| `xbox360/hud_panel.c`, `hud_panel.h` (new: canvas and layout only) | WP1.2 |
| `xbox360/panel_360.c`, `panel_360.h` (new: gestures, cursor, row logic, publish) | WP1.3 |
| `xbox360/pad_quirks.h` (new: per-pad Share byte table) | WP2.1 |
| `xbox360/voice_360.c`, `game_360.c`, `diag_360.h`, `core/gip.c`, audio region of `xw_plugin.c` (lines ~1000-2270), `voice.py`, AUDIO/DIAG parts of `xw.py` | WP1.4, then WP1.5/1.6 (same engineer, serial) |
| `xbox360/xinput_360.c/.h` (menu removal) | WP1.3 |
| Reload region of `xw_plugin.c` (~2825-3420), `handover.h`, reload part of `xw.py`, `docs/HOT-RELOAD.md` | WP3.x |
| `xbox360/team_360.c/.h`, `net_360.c/.h`, `team.py` (new) | WP4.x |
| `xbox360/hud_360.h` | Frozen by this plan (section 1.0). Changes go through the coordinator. |
| `xa.py` (Xbdm class), `xbox360/accept/` runner, `xw.py tee` | WP0.2 |
| `build.sh`, `detour.c`, `thunks_*.s`, `XwThreadC` main loop, `do_release` call list | WP0.1 only. Later packages get their hook-up lines pre-wired there (section 0.1). |

### WP0.1 Integration base (one engineer, serial, first)

Files: `build.sh`, `detour.c`, `thunks_common.s`, `xw_plugin.c`, `hud_360.c`, `hud.py`, `research/HUD.md`, `CLAUDE.md`.

1. Bring `hud_360.c`, `hud.py` and `research/HUD.md` from 4266a69 onto `headset-audio`. HEAD's `build.sh` already picks `hud_360.c` when present, and HEAD already calls `hud_init/hud_poll/hud_quiesce`.
2. Port the `names_reg` `case 14: case 15` fix into HEAD's `detour.c`. Keep `TRAMPOLINES 12`.
3. Add `hud_quiesce`/`hud_resume` to `hud_360.c`: a `g_quiesced` flag; `XhSwapC` records nothing and only returns the trampoline; `hud_poll` draws nothing.
4. Set `g_xh.enable` to 0 by default so VdSwap is not hooked at boot until WP1.1 says so.
5. Mechanical move, no behaviour change: cut `hud_update`, `share_button`, `share_controls` and `diag_*` (xw_plugin.c 2404-2622) into `panel_360.c` behind the `panel_360.h` contract below. `xw_plugin.c` keeps only the calls.
6. Pre-wire empty call sites so later packages never touch the main loop:
   - `xt_init()`, `xt_poll()`, `xt_quiesce()`, `xt_resume()` from a `team_360_stub.c`;
   - `panel_poll()`;
   - `trace(code)` no-op macro at the reload steps.

Acceptance on the console (one reboot, one button press):
- `xw.py status`, `voice.py status` and `hud.py status` all read their blocks.
- `hud.py status` says not hooked and enable 0.
- Halo 3 voice, game audio, Share tap and Share + D-pad behave exactly as on 5eabd0a.
- `out_starved` stays 0 and `mic_pps` stays 1000 over 60 s.

### WP0.2 Tooling (one engineer, parallel with WP0.1; Python only)

Files: `xa.py`, `xw.py` (new subcommands only), `xbox360/accept/__init__.py`, `accept/run.py`.

- `xw.py tee [DIR]`: polls every loaded xw build's log ring plus the `XW36`, `XWHO`, `XWAU` and `XH36` heartbeat and trace words every 50 ms. It appends to `out/logs/<timestamp>.log` and prints `CONSOLE SILENT since <t>` when xbdm stops answering. This is the only way a log survives a freeze.
- `Xbdm.screenshot()` and `hud.py shot FILE.png`: use xbdm's `screenshot` command to fetch the front buffer, de-tile it and write a PNG. `hud.py shot --find` reports whether the panel's 4 sentinel texels are present and where. Unverified: that this xbdm.xex build implements `screenshot`. If it does not, fall back to `getmem` of `d_base` (the address the HUD status already reports) with the same de-tile code.
- `xw.py hooks`: prints the first 4 words of each hooked function (4 XInput, 3 XVoiced, VdSwap), says whether each is original code, our slot jump, or a foreign patch, and names the module that owns the jump target.
- `accept/run.py <name...>`: runs acceptance scripts with the tee running, and collects PASS/FAIL.

Acceptance: `xw.py tee` runs across a deliberate `xw.py usbreset 0` and the log file contains the re-enumeration; `xw.py hooks` lists 7 functions as "ours".

---

## Phase 1: MixAmp parity, one headset

### 1.0 Decisions

- **Gesture model.**
  - Share tap (released in under 400 ms, no other button): mic mute toggle, with a small "MIC MUTED / MIC ON" toast for 1.5 s. Was 1000 ms.
  - Share held 250 ms or longer, or any D-pad press while Share is down: the panel shows.
  - Panel hides on Share release.
  - Long hold and release with no other button: nothing. The XAM menu is gone.
- **Navigation.** D-pad Up/Down moves the cursor; Left/Right changes the row. The cursor always starts on Balance each time Share goes down, so Share + Left/Right is still balance. Volume becomes Share, Down, Left/Right.
- **LB/RB dropped.** Sidetone is a row. Bumpers pass through to the title while Share is held. One model is easier to explain at a LAN.
- **D-pad masking** from titles while Share is down stays as is (`ctl_mask` logic).
- **Rows** (6 in Phase 1; 2 more reserved for Phase 4):

| Row | Range | Default | Backing |
|---|---|---|---|
| BALANCE (game/chat) | -5..+5, centre tick | 0 | `xg_adjust` |
| VOLUME | 0..10 | 10 | `xg_adjust` |
| SIDETONE | 0..10 | 0 until WP1.5 passes, then 3 | `xg_adjust` |
| MIC LEVEL | 1..10 (x10 %) | 5 | `g_xd.mic_gain` (extend from 3 steps to 10) |
| NOISE GATE | auto / off / 1..4 (-60 -55 -50 -45 dBFS) | auto | `g_xd.gate` |
| TEST TONE | off / 440 / 1k | off; forced off when the panel hides | `g_xd.tone` |

- **Game audio on/off is not a row.** Balance +5 already gives chat only. It stays on xbdm (`voice.py game on|off`).
- **Live mic meter** in the panel: a 10-segment meter plus a gate-open dot beside MIC LEVEL and NOISE GATE. It lets a player set the gate without a Mac.
- **Panel background is opaque by default** (`keep` = 0: write-only, no front-buffer reads). The translucent blend stays as an xbdm option. Reason: the panel grows from 480x56 to about 480x200 px at 720p, drawn every frame on the driver thread (hardware thread 2, shared with Usbd DPCs and audio). Reads of a possibly write-combined front buffer are the cost HUD.md section 5 warns about.
- **Settings stay in memory only** in Phase 1. Compiled defaults mean no setup after a reboot. Persisting to a file is listed under Later; surviving a hot reload is WP3.4.

### 1.0.1 Contract: `hud_360.h` version 2 (frozen)

```c
enum { HUD_HIDDEN = 0, HUD_TOAST = 1, HUD_PANEL = 2 };          /* hud_state.visible */
enum { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC, HUD_ROW_GATE,
       HUD_ROW_TONE, HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROWS };

struct hud_state {
	u8 visible;      /* HUD_* */
	u8 muted, flash, headset, game_on;
	u8 cursor;       /* HUD_ROW_*: the selected row */
	u8 rows;         /* bit n = row n is shown (Phase 1: 0x3F) */
	s8 balance;      /* -5..+5 */
	u8 volume, sidetone;        /* 0..10 */
	u8 mic_level;    /* 1..10 */
	u8 gate;         /* 0 off, 1 low, 2 high */
	u8 tone;         /* 0 off, 1 440 Hz, 2 1 kHz */
	u8 mic_vu;       /* 0..10: the mic's level now (after gain, before the gate) */
	u8 gate_open;    /* 1 = the gate is passing the mic */
	u8 team_level;   /* 0..10 (Phase 4) */
	u8 channel;      /* 0 off, 1..8 (Phase 4) */
	u8 team_peers;   /* consoles heard on this channel (Phase 4) */
	u8 team_talking; /* bit n = peer n is talking (Phase 4) */
	u8 reserved[5];
};
_Static_assert(sizeof(struct hud_state) == 24, "hud.py test state");

void hud_publish(const struct hud_state *s);   /* any thread; copied under a seqlock */
void hud_init(void);  void hud_poll(void);     /* driver thread */
void hud_quiesce(void);  void hud_resume(void);
```

### 1.0.2 Contract: `hud_panel.h` (between WP1.1 and WP1.2)

```c
#define HP_W 240          /* canvas units; drawn at integer scale 1/2/3 */
#define HP_H_MAX 104
enum { HP_BG, HP_LABEL, HP_BRIGHT, HP_DIM, HP_ACCENT, HP_WARN, HP_COLOURS };
struct hp_canvas { u16 w, h; u8 px[HP_W * HP_H_MAX]; };  /* palette indices; 0xFF = transparent */
/* pure function: no globals written, no kernel calls, integer only; host-testable */
void hp_build(struct hp_canvas *c, const struct hud_state *s);
/* the 4 sentinel positions (canvas units) hud_360.c uses for its repaint check */
void hp_sentinels(const struct hud_state *s, u16 xy[4][2]);
```

### 1.0.3 Contract: `panel_360.h` (between xw_plugin.c and the controls)

```c
/* all on the driver thread */
void panel_init(void);
/* a raw GIP input packet (command 0x20) of port/pad, before gip_rx parses it */
void panel_raw_input(int pad, u16 vid, u16 pid, const u8 *pkt, int len);
/* the parsed state, before xa_xinput_update: masks the controls the panel consumed */
void panel_filter(int pad, struct gip_gamepad_state *st);
void panel_pad_reset(int pad);   /* a new controller in this slot */
void panel_poll(void);           /* every pass: timers, tone auto-off, hud_publish */
```

xbdm block `XWPN` (panel_360.c):
- controls: `inject` (bit 0 Share, 1 Up, 2 Down, 3 Left, 4 Right, applied as pad 0's input), `inject_pad`;
- state: `visible`, `cursor`, `share_down`, `taps`, `holds`, `adjusts[HUD_ROWS]`, `masked_presses`, `tone_auto_offs`.

`inject` lets the coordinator drive the whole panel over xbdm without touching the pad.

### WP1.1 HUD hook bring-up on hardware (engineer A)

Files: `hud_360.c`, `hud.py`, `research/HUD.md`. Depends on WP0.1 and WP0.2.

Code changes:
- Replace the 20 s timed install with explicit stages in `g_xh.enable`: 0 nothing; 1 check only (resolve ordinal 603, compare the 23 signature words, report `sig_ok`, patch nothing); 2 install the hook and record swaps but never draw; 3 draw allowed. `hud.py stage N`.
- Split the canvas builder out to WP1.2's `hp_build`. `hud_360.c` keeps the hook, the swap records, fetch decode, tiling, blit, flush and status.
- Variable panel height from `hp_canvas.h`. Geometry is clamped to the title-safe area and falls back to scale 1 if it does not fit.
- `keep` = 0 path: no reads except the 4 sentinel texels.
- New status counters:
  - `sig_ok`, `install_ms`;
  - `swaps_per_s`, `draws_per_s` and `cover_pct` (draws / swaps while visible);
  - `fence_late_us` (fence seen to draw start) and its max;
  - `d_ticks` histogram in 4 buckets (<0.25, <0.5, <1, >=1 ms);
  - `skip_*` already exist;
  - `titles_seen` and the last 4 title handles with their decoded format, endian, tiled and size;
  - `quiesced`.
- Each stage change and each new (title, format) pair gets one log line.

Staging on the console. Each step is one `hud.py` command; the tee is running; hold 30 s between steps.

| Step | Command | Expect | If not |
|---|---|---|---|
| H0 | boot, `hud.py status` | enable 0, not hooked | - |
| H1 | `hud.py stage 1` | `sig_ok 1`, address 0x800C8B90 | stop: not the code read offline |
| H2 | at idle Aurora: `hud.py stage 2` | "VdSwap hooked"; `swaps` grows about 60/s; console alive 60 s; decoded fetch printed | freeze here = the patch or the stub; reboot and compare the stub's frame against VdSwap's stack arguments 9 and 10 |
| H3 | `hud.py status` x3 | `fence_same` behaviour recorded; format 8_8_8_8 or 2_10_10_10; tiled; 1280x720 | unknown format: add the decode, no drawing yet |
| H4 | `hud.py stage 3`, `hud.py show`, `hud.py shot --find` | sentinels found; PNG shows the bar | no pixels but `draws` grows: `set mode 1`, then 2; wrong colour: `swap_rb 1` |
| H5 | 20 screenshots over 10 s | sentinels in 19 or more of 20 | flicker: compare modes 0/1/2 by this ratio |
| H6 | launch Halo 3 (magicboot), repeat H3-H5 in the menu and in a match | same; `cover_pct` >= 95; `d_ticks` max < 50,000 (1 ms at 50 MHz) | see fallback |
| H7 | panel up 60 s in a match | `out_starved` +0, `mic_pps` 1000, `out_frames_missed` +0 | see fallback |
| H8 | title switch with the panel forced on (Halo 3 to Aurora and back) | no freeze, `skip_title` counts | - |

Fallback, pre-specified, only if H6 or H7 fails: move drawing to its own system thread (`XhDrawThread`, NV64 thunk, pinned to a hardware thread other than 2 and 4, priority below the driver thread). `hud_poll` then only handles stages. The seqlock and the swap ring are already safe for that.

the owner check: at H4 and H6, "is the bar steady, readable, and inside the picture?"

Known residual risk (accepted): the first patch of VdSwap happens while a render thread calls it 60 times a second. The `b .` parking does not protect a thread already inside the first four instructions, a window of tens of ns per frame, once per boot. Hot reloads never re-patch.

### WP1.2 Panel renderer (engineer B; parallel with WP1.1)

Files: `hud_panel.c`, `hud_panel.h`, `hud.py` (the `show` keys only: coordinate with A, or submit as a patch to A).

- `hp_build` layouts:
  - **toast** (HUD_TOAST): one line, `MIC MUTED` or `MIC ON`; about 120x14 units.
  - **panel** (HUD_PANEL): a title line (`HEADSET`, with `MIC MUTED` in a box on the right when muted), then one line per row in `rows`. Each line is label, control, value. The selected row gets an accent bar on the left and bright text. The mic meter and gate dot sit on the MIC LEVEL and NOISE GATE lines.
  - **no headset**: `NO HEADSET` only.
- Font: the existing 5x7 set covers every label. Add `<` and `>` for the selected row's arrows.
- A host test (the existing `HUD_HOST_TEST` path) renders every state to PNG. That is for the engineer only; it is not the acceptance.

Acceptance on the console, after WP1.1 reaches H4: `accept/p1_panel.py` runs `hud.py show` for 12 states (each row selected, extremes, muted, no headset, toast). It takes a screenshot of each and checks the sentinels and the accent bar's row position by pixel. the owner check: one look at the full panel in Halo 3 for legibility at his seat.

### WP1.3 Controls and XAM menu removal (engineer C; parallel with 1.1 and 1.2)

Files: `panel_360.c/.h`, `xinput_360.c/.h`, `accept/p1_controls.py`. Depends on WP0.1.

- Implement section 1.0's gesture model and the `XWPN` block with `inject`.
- Row adjust calls: `xg_adjust` for balance, volume and sidetone; write `g_xd.mic_gain`, `g_xd.gate`, `g_xd.tone` for the rest (WP1.4 widens their ranges; until then clamp to today's values).
- Centre tick on balance stays (`xv_tick`).
- Test tone is forced to off when the panel hides, and on pad removal.
- Publish `hud_state` v2 on every change, and at 20 Hz while the panel is visible (for the meter).
- Share detection goes through `pq_share_down(vid, pid, pkt, len)` from `pad_quirks.h`. C creates that file with the one known entry (045e:0b12: packet byte 22 bit 0, min length 23, `pkt[3] >= 19`). WP2.1 adds the SCUF entry.
- Remove from `xinput_360.c/.h`: `xa_xinput_menu`, `xa_xinput_take_menu`, `do_menu`, `menu_poll`, `REQ_MENU`, the `XamShowMessageBoxUI` resolve, and the menu notes in `xa_xinput_quiesce/shutdown`. Remove `diag_*` from `panel_360.c` and the `menu_*` fields' use. The `xd_status` layout is WP1.4's; C only stops writing them.
- Keep `XA_NOTIFY_MIC_MUTED/ON` behind an xbdm flag, default off, because the toast replaces them.

Acceptance (`accept/p1_controls.py`, no hands needed; uses `inject` and reads `XWPN`, `XVGA`, `XWDG`, `XVHS`):
- tap toggles mute, and `XVHS` mute follows;
- a 300 ms hold shows the panel, and `hud.py status` reports `pub_visible`;
- Left x3 gives balance -3; Down then Right gives volume +1; and so on for each row;
- with the panel hidden, D-pad reaches `g_xw.pads[0].buttons` as seen by the title; with Share down, it is masked;
- tone is off after release;
- a 2 s hold and release changes nothing;
- no `XamShowMessageBoxUI` symbol in `out/xw.exe`.

the owner check: 2 minutes in Halo 3 with the real Share button: tap mutes, hold shows the panel, D-pad doesn't move the in-game menu while Share is held.

### WP1.4 Audio settings, defaults, knob removal (engineer D; parallel)

Files: `voice_360.c`, `game_360.c/.h`, `diag_360.h`, `core/gip.c/.h`, `xw_plugin.c` (audio region and `struct xw_audio_status` only), `xw.py` (AUDIO_FIELDS, DIAG_FIELDS, `diag`, `earfb`, `dump`), `voice.py`, `research/AUDIO.md` 8.6.

- **Mic level:** 10 % to 100 % in 10 % steps (was 100/50/25). Q15 gain, ramped over 10 ms as now.
- **Defaults:** `gate` = 1 (low), `mic_gain` = 50. Sidetone stays 0 until WP1.5.
- **Mic meter:** export `u32 xd_mic_vu(int pad)` (0..10 from the existing 20 ms RMS, log-ish table) and `bool xd_gate_open(int pad)`.
- **Remove:**
  - `ear_fb` modes, `ear_credit`, `ear_skips`, `gip_headset_send_x/_y`, `send_audio_v` if nothing else uses it;
  - `dpc_refill`, `staged`, `AUD_REFILL_INFLIGHT`;
  - the `out_blocks` knob (fixed at `AUD_OUT_BLOCKS` = 5);
  - the tap and resampler dumps (`XG_DUMP_*`, `xg_dump_*`);
  - the `menu_*` fields of `xd_status`.
- **Keep, xbdm only:**
  - `micdump`;
  - one earpiece dump (`xw.py dump`, final earpiece PCM, allocated on first use);
  - `buffer_ms`;
  - the tone and its level;
  - all OUT/IN counters (`out_starved`, `out_frames_missed`, the flow fields);
  - `mic_skips` as a counter.
- Bump the `XWAU` and `XWDG` block versions. Update the `_Static_assert`s and the Python field lists in the same commit. `xw.py` refuses a block whose version it doesn't know, instead of mis-reading it.

Acceptance (`accept/p1_audio.py`): after boot with no commands:
- `XWDG` shows gate low and mic 50 %;
- `xw.py diag micgain 30` applies;
- `gate_opens` rises when the owner talks and `gate_open` drops to 0 in silence (the owner check: 10 s talk, 10 s quiet);
- 5 minutes in a Halo 3 match: `out_starved` +0, `out_frames_missed` +0, `mic_pps` 1000 +/- 2, `underruns` +0;
- `micdump` and `dump` still produce WAVs.

the owner check: Halo 3 teammates no longer hear a hot mic with zero setup.

### WP1.5 Sidetone verification (engineer D, after WP1.4)

Files: `voice_360.c` (sidetone section), `accept/p1_sidetone.py`.

- Add to `XVHS`: `side_delay_ms` = FIFO fill + OUT blocks in flight x 8 + one mic block. This is computed, and it is the number to show the owner.
- Test:
  1. `voice.py game off`, no chat, sidetone 5.
  2. Arm `micdump` and the earpiece `dump` together while the owner counts to five.
  3. The script cross-correlates the two WAVs. It reports the lag at fill time (add 40 ms for the OUT queue), the level ratio against the expected 50 % x master, and guard trips.
- Test: sidetone 10 with the headset on the desk next to the mic: the feedback guard must trip (`guard trips` > 0) and no sustained howl (earpiece RMS falls within 1 s).
- Test: muted gives no sidetone. Mic level and gate do not change the sidetone, as designed.

the owner check: "does your own voice sound like an echo?" If yes, go to WP1.6. If acceptable, set the default sidetone to 3.

### WP1.6 Low-latency sidetone (conditional; engineer D)

Only if WP1.5's the owner check fails.

Idea: mix the sidetone late, into OUT blocks that are already queued but whose frames have not gone out yet, instead of at fill time. The block buffers (`aud_out_buf`) are ours, and the controller reads them at each frame. Target about 15-20 ms (one mic block plus 1-2 frames of margin).

Needs:
- the current USB frame number and each queued block's start frame, so the code knows which bytes are still ahead. Read from the isoch completion (frames done) plus the timebase. The endpoint's next-frame field is a kernel internal and unverified.
- a cache flush of the touched lines (`dcbf`).
- a 2-frame guard so a frame being transmitted is never edited.

Gate it (`xw.py diag latemix 0|1`), default off. Acceptance: the same cross-correlation, lag <= 25 ms, `out_starved` +0, no clicks (the owner). If it cannot be made clean, ship fill-time sidetone with default 0 and say so.

### Phase 1 risks

| Risk | Mitigation |
|---|---|
| VdSwap patch or stub freezes the console | Stages H1-H2 separate "signature ok" from "patched" from "drawing"; `enable 0` default until H8 passes |
| Fence is not a usable "frame is in memory" signal | Modes 1 and 2 exist; H5's screenshot ratio picks the mode per evidence |
| Drawing on the driver thread starves audio (ticks return) | Opaque panel; H7 gate on `out_starved`; draw-thread fallback specified |
| Title unmaps its front buffer between the check and the write | Existing per-page check; accepted, the window is microseconds |
| Halo 3 uses a front-buffer format not decoded | Counted and skipped, never guessed; H6 records it |
| Status struct and Python list drift | Version bump and assert in the same commit; tools refuse unknown versions |
| Sidetone delay | WP1.5 measures it, WP1.6 is the fallback, default stays 0 otherwise |

### Phase 1 definition of done

On a cold boot with no xbdm command, with the Series pad and headset in Halo 3:
- game audio and chat are in the headset;
- Share tap mutes, with a toast;
- holding Share shows our panel with 6 rows that all work from the D-pad;
- the mic is gated and at 50 %;
- there is no XAM box anywhere;
- 30 minutes of play with `out_starved` 0 and `mic_pps` 1000;
- sidetone is either verified and on by default, or documented as off with the measured delay;
- all `accept/p1_*.py` pass.

---

## Phase 2: SCUF Instinct Pro (2e95:0504)

### WP2.1 Console verification and quirks (engineer C or E; after WP1.3 for the table, otherwise independent)

Files: `pad_quirks.h`, `core/gip.c` (only if format choice or start-up needs it; coordinate with D), `docs/WIRED.md`, `accept/p2_scuf.py`.

The SCUF has never been on the console. Steps, each read over xbdm with the tee running:

| Step | Do | Record |
|---|---|---|
| S1 | Plug the SCUF into a rear root port (no hub), switch it on. `xw.py status` | `match_calls`, `adds`, VID/PID, announce or "started blind", identify, firmware, "pad N ready" |
| S2 | `xw.py debug 1`, replug | Security handshake result ("client 0 authenticated" or the failure). Input is marked ready regardless. Microsoft pads only expose the headset after the handshake; whether the SCUF does is unknown. |
| S3 | `xw.py test` | Every standard control ticks |
| S4 | `xw.py raw 60`: the owner presses Share, each paddle and the profile button, one second apart | The byte and bit for each. `NOISY` in `xw.py` already excludes bytes 40-46 except 43 for this pad. |
| S5 | Plug the headset in. `xw.py status` | Client 1 announce, class string, the format pairs, the chosen pair, the iso endpoints and max packets from the config descriptor (`desc_source` 1 = read, 2 = the 0x83/0x03 guess) |
| S6 | First run staged: `xw.py audio 1`, `hsrestart`; then 2, 3, 4 | Open statuses, the first IN block's per-frame status and lengths, `mic_pps`, `out_starved`, the flow field |
| S7 | `xw.py audio 5` (self-test tone) then `micdump` | Tone amplitude in the mic analysis if the owner holds the earcup to the mic; WAV sanity |
| S8 | Stage 4, Halo 3 | Voice both ways, game audio, the panel from the SCUF's Share button |

Code that will likely be needed:
- A `pad_quirks.h` entry for 2e95:0504 with S4's Share byte and bit.
- Paddles: the Instinct normally sends them as remapped standard buttons. If S4 shows separate bits, leave them unmapped. They are out of scope.
- Format choice: today the first advertised pair is used (`core/gip.c:939`). If the SCUF's first pair has a rate `voice_360.c`'s `rate_ok` refuses, or an OUT packet above the endpoint's max, add `hs_pick_format`: first pair whose rates pass `rate_ok` and whose 1 ms fragment + 4 fits `aud_max_out`, preferring stereo out.
- If the handshake fails and no headset announces: try `gip_headset_probe` after ready (the path hot reload already uses) before concluding the jack needs auth.
- If the pad stays dark at boot: same as the Series pad, the Xbox button. Record whether the blind-start power-on wakes it (`fallback_rx`).

Only one headset streams at a time (`g_audio.owner`): with both pads plugged in, the first headset wins. That is the 12-iTD pool limit, not a bug.

Acceptance: `accept/p2_scuf.py` checks S1-S6 counters (`mic_pps` 1000 +/- 2, `out_starved` +0 over 60 s, format logged). the owner check for S7-S8.

Risks:
- No headset without auth: try the probe; otherwise document "SCUF jack unsupported".
- Different max packet or rates: `hs_pick_format`.
- A Share report in a different GIP command than 0x20: `xw.py debug 2` shows it, and `panel_raw_input` would need that command too.

### Phase 2 definition of done

SCUF on the console: all inputs, Share drives mute and the panel, the headset streams with the same counters as the Series pad, 30 minutes of Halo 3. Or a written finding saying exactly which step fails and why, with the log.

---

## Phase 3: hot reload that doesn't freeze

Two freezes, no logs: one about 10 s after "done", one at "take over". The plan is evidence first, then the fix.

### 3.1 Likely causes, ranked from the code

| # | Cause | Why it fits | Cheapest test |
|---|---|---|---|
| 1 | **Take-over overlaps the loader's title switch.** `xw.py` writes `cmd_takeover` the moment `xw<n>.xex` is in the module list; the loader sleeps 1 s and launches Aurora. Hook stores, the XAM thread's first notification, the class-driver link, `adopt_dpc`, the GIP identify, the XAM binds, SET_INTERFACE and iso opens then all land during Aurora's start. Boot avoids exactly this with an 8 s settle; the reload path skips it (`xw_boot_grab && !g_pred`). | Fits both freezes ("at take over"; "about 10 s after" is Aurora still starting). The repo's own docs say an overlap "can hang the console". | Delay `cmd_takeover` until the title handle has been stable 10 s. Or use `--in-place` at idle Aurora (no title switch at all). |
| 2 | **Isochronous re-open after a close runs the 12-iTD pool dry.** One headset uses 10 of 12. If the closed endpoints' TDs are not back in the pool when the new build opens, the queue path takes a NULL TD on Usbd's DPC: a hard freeze with no log. Close then reopen with 5 OUT blocks has not been exercised much. | Fits "about 10 s after done": pad ready, handshake, 1.5 s probe delay, identify, format, power-on retries, open, stream. | No reload needed: on the boot build, `xw.py hsrestart 0` x10 and `xw.py audio 0` / `audio 4` + hsrestart x5. If this freezes, the reload is innocent. |
| 3 | **MEC unregister then a second register.** `XAudioUnregisterRenderDriverMECClient` was never called before the first reload; the new build registers 20 s after its thread started, which lands roughly 10-15 s after "done". | Timing fits freeze 1. | `voice.py game off` before the reload (nothing registered, nothing to unregister). |
| 4 | **A foreign hook on one of our 7 functions (HaloLAN, JRPC2).** If another plugin patched over our slot jump, `jumps_through` fails, adoption is skipped, and `detour_install` relocates and re-patches live code. | Would hit at "take over" (XInput hooks first). | `xw.py hooks` before any reload. |
| 5 | **Two interrupt IN transfers on one endpoint** (the known freezer) through a hole in the `in_busy`/`in_wait`/`in_fwd` hand-off, or a second reload's chain. | Would hit a few ms after the identify answer. | A guard in `queue_in` (refuse and count if one is outstanding or `in_wait`) plus a trace. |
| 6 | **The pad resets itself on the second security handshake.** RemoveDevice then arrives for an adopted node and closes handles the new build only copied, with the old image's IN transfer still queued. | Possible at take over + 1-2 s. | The trace shows `remove` right after `adopt`. `-DXW_NO_AUTH`-style skip of the second handshake on adoption. |
| 7 | The endpoint object does keep a pointer to the opening block (the "writes only the first word" reading is from disassembly of the OHCI open only). | Would hit on the first queue through a copied handle. | Trace around the first OUT and first IN queue after adoption. `--reset` mode avoids copied handles entirely. |
| 8 | The old XAM thread or driver thread ending while XAM still refers to it (a queued notification, an open message box's overlapped). | Low. Menu removal (WP1.3) removes the box case. | Trace. |

### WP3.0 Pre-flight (coordinator + engineer F; no build needed)

1. `xbdm modules`: record every module's base and size. Confirm nothing sits in 0x92800000-0x94000000. Record HaloLAN.xex's base.
2. `xw.py hooks` (WP0.2): all 7 functions must jump through our slots. Fetch `HaloLAN.xex` with `getfile` and list its imports to see whether it touches XInput, XVoiced, VdSwap or NetDll.
3. Cause 2's test on the boot build: `hsrestart` x10 with the tee running, then `audio 0/4` cycles.
4. Record whether xbdm still answers after a freeze. `xw.py tee` prints it. If xbdm survives, the freeze is a thread spinning, not a DPC, and `xbdm threads` / `getcontext` give the PC. If xbdm is dead, it is DPC level or the kernel.

### WP3.1 Instrumentation and staged gates (engineer F)

Files: `xw_plugin.c` (reload region, plus two gate checks), `handover.h`, `xw.py` (reload), `accept/p3_reload.py`. Can be written while Phase 1 is in progress; no file overlap.

- **Trace.** Use two of `xw_handover.reserved[8]` (append-only rule kept; `XW_HO_VERSION` stays 1): `trace` (the last step code) and `trace_ring` (address of a 64-entry ring of `{code, timebase}` written with one store each, safe on a DPC). Codes are placed before and after every step in `do_release`, `commit_dpc`, `takeover_import`, `xa_xinput_init`, the link DPC, `adopt_dpc`, `XwInForwardC`, the first `queue_in`/`tx_submit` on an adopted port, `pad_ready`'s bind, `aud_open_dpc`, `try_register`, `install_hooks`, `XwRemoveDeviceC`. `xw.py tee` polls it at 50 ms.
- **Gates.** One more reserved word, `cmd_gate` (default 99 = run through). The build waits at each numbered step until `cmd_gate >= n`:

| Gate | Side | Step |
|---|---|---|
| R1 | old | audio blocks drained |
| R2 | old | headset off, endpoints closed, alternate 0 |
| R3 | old | MEC unregistered |
| R4 | old | pads and headsets unbound, XAM thread drained |
| R5 | old | commit on Usbd's DPC |
| R6 | old | XAM thread and driver thread end |
| L1 | - | module loaded, idle, waiting for `cmd_takeover` (already how it works) |
| T1 | new | handover block checked, hooks inherited (registry copy only) |
| T2 | new | XInput: XAM thread started, 4 slots stored |
| T3 | new | class driver linked |
| T4 | new | `adopt_dpc` |
| T5 | new | IN started (existing `go` 4 and 5) |
| T6 | new | packets processed, `gip_adopt` sent (existing `go` 6) |
| T7 | new | pad bound in XAM (new check before `xa_xinput_bind`) |
| T8 | new | XVoiced slots stored (hold with `voice.py stage 0`, then raise) |
| T9 | new | headset probe, SET_INTERFACE, iso open, stream (existing `xw.py audio 0..4`) |
| T10 | new | MEC registered (`voice.py game off`, then on) |
| T11 | new | VdSwap slot stored (`hud.py stage`) |

  The slot build already waits for `cmd_takeover` before doing anything, so `xw.py` can write `go`, the audio stage, the voice stage, game enable and the HUD stage into the new image first. Most gates are the existing ones.
- **`xw.py reload --step`**: prints each gate, waits for Enter (or `--hold SECONDS`, default 15, for unattended runs), prints the trace and the new log lines between steps.
- **`xw.py reload --settle`**: wait until `XexExecutableModuleHandle` has been stable 10 s before `cmd_takeover`.
- **Guards** (cheap, permanent): `queue_in` refuses and counts `in_double` if a transfer is outstanding or `in_wait` is set; `adopt_dpc` logs each copied handle.

### WP3.2 Experiments (coordinator at the console, engineer F reading logs)

Each successful reload is the next experiment's baseline, with no reboot. Each freeze costs a reboot and a button press, so run the cheapest discriminators first. Always: idle Aurora, tee running, `xw.py hooks` clean.

1. **E1, minimal path.** Before the release: `xw.py audio 0` + `hsrestart` (headset off), `voice.py game off`, HUD stage 0. `reload --step --settle`. This is a reload with no audio, no MEC and no title-switch overlap. If it passes, causes 5-8 are unlikely and the base mechanism works.
2. **E2, add the headset** to the now-running slot build: `xw.py audio 4` + `hsrestart`. That is cause 2 in isolation (a fresh open in a new image after the old one closed).
3. **E3, reload with the headset streaming** (gates R1-R2 and T9 do real work), game audio still off.
4. **E4, game audio on**, then reload (R3 and T10: cause 3).
5. **E5, without `--settle`** (cause 1 on purpose). A freeze here with E1-E4 clean confirms it.
6. **E6, `--in-place`** at Aurora, then in a Halo 3 match.
7. **E7, three reloads in a row** without touching the pad (the forwarded-IN chain).

If E1 itself freezes: the trace's last code names the step. Then re-run with `--reset` (no copied handles: cause 7) and with the second handshake skipped (cause 6).

### WP3.3 The fix (engineer F; scoped by what E1-E7 show)

Expected, in order of likelihood:
- **Cause 1:** the slot build itself waits for a settled title before T2 (reuse `wait_for_title_switch` logic, without the unrecognized-port rescan). `--in-place` becomes the default once E6 passes, since it has no title switch. The loader path stays as a fallback.
- **Cause 2:** before opening, read the kernel's iTD free count and refuse the open (audio stays off, logged) if fewer than 10 are free. Retry every second for 5 s. The free-list head address must be found in the 17559 dump first; that is a research sub-task. If the close really leaks, keep the headset's endpoints open across the reload and hand their handles over like the interrupt pair (append `h_aud_in`/`h_aud_out` to `xw_ho_port`; that is a meaning change, so bump `XW_HO_VERSION`).
- **Cause 3:** hand the MEC handle to the next build instead of unregistering. The registered callback is a slot-style indirection in the old image (one store to retarget), the same idea as the detours.
- **Cause 4:** `detour_install` refuses to patch a function whose first words are neither the 17559 originals nor our jump. `xw.py reload` runs the `hooks` check first and stops.
- **Causes 5-7:** per the trace.

Update `docs/HOT-RELOAD.md` (the Unverified list becomes Verified or Changed) and `CLAUDE.md`.

### WP3.4 Settings survive a reload (engineer F; small; after 3.3)

Append `settings` to the handover block: the address of one struct `{balance, master, sidetone, mic_gain, gate, mute[4], team_level, channel}`. The new build copies it after T1. Acceptance: set balance -3 and gate high, reload, both still set.

### Phase 3 risks

- Every failed experiment costs the owner a button press: E1 is designed to be the most likely to pass, and WP3.0 costs nothing.
- The freeze may kill xbdm before the last trace poll: gates hold 15 s per step, so the last polled code is the step that froze.
- A fix for one cause may hide another: E7 and the acceptance loop below.
- Each reload leaves about 700 KB loaded: 24 slots. Reboot when the count gets high.

### Phase 3 definition of done

`accept/p3_reload.py`: 10 consecutive `xw.py reload` runs (5 at Aurora, 5 `--in-place` inside a Halo 3 match). Each is followed by 60 s of checks: pad input counters moving, headset streaming with `mic_pps` 1000, game audio registered, HUD hook adopted, `in_double` 0. No freeze. The pad's light never goes out. Or a written finding that names the step that cannot be made safe, with `--reset` or a reboot as the documented path.

---

## Phase 4: console-to-console team voice (the daisy chain)

Separate from HaloLAN for now. All of it is new ground: nothing in this repo has opened a socket. `research/DISCORD.md` 4.2 has the only notes, and they are from Xenia's sources and public documentation, not from the console.

### 4.1 Design

**Transport.** UDP on the LAN from a new thread inside xw.xex.
- *API:* XAM's `NetDll_*` exports, which take a caller type as their first argument. Use the system caller (`XNCALLER_SYSAPP` = 2) so a title's own `XNetCleanup`/`WSACleanup` does not close our socket. xbdm.xex and JRPC2.xex are resident on this console and use sockets while titles run, so the model exists. Unverified here: the ordinals (from Xenia's table: WSAStartup 1, socket 3, closesocket 4, ioctlsocket 6, setsockopt 7, bind 11, select 15, recvfrom 20, sendto 24, XNetStartup 51, XNetGetTitleXnAddr 73, XNetGetEthernetLinkStatus 75). Resolve by ordinal and check each against the XAM dump's export table in `research/dumps/`.
- *Plain UDP:* XNet sockets are secure (encapsulated) by default. Homebrew marks a socket insecure with `setsockopt(SOL_SOCKET, 0x5801, 1)`. `launch.ini` on this console has `sockpatch = true`. Whether a system caller needs either is unverified.
- *Raw sockets* are not needed and not planned.
- *Broadcast, not multicast or unicast, for v1:* one `sendto` to the subnet broadcast address per packet, `SO_BROADCAST` set. Reasons:
  - no peer table is needed to talk;
  - a PC or phone can listen to every channel without joining anything (OBS, admin);
  - system link already depends on broadcast working on these LANs;
  - the load is trivial: a talker is 32 kB/s, and the gate stops sending in silence.
  - Multicast needs IGMP support in the 360's stack, which nothing confirms.
  - Unicast fan-out is the fallback if broadcast audio turns out lossy on some switch; the header has a flag for it.
- *Port:* UDP 3660. Not 3074 (system link, Live), 730 (xbdm) or 1000-1001.

**Wire format** (big-endian; PCM is s16 big-endian, the console's native order):

```c
#define XT_MAGIC 0x58575456 /* "XWTV" */
#define XT_VERSION 1
#define XT_PORT 3660
enum { XT_HELLO = 1, XT_AUDIO = 2, XT_BYE = 3 };
#define XT_F_TALKING 1
#define XT_F_MUTED   2
#define XT_F_PC      4   /* a PC or phone peer, not a console */
#define XT_F_ALLCALL 8   /* heard on every channel (admin talk-back) */
struct xt_hdr {          /* 20 bytes */
	u32 magic;
	u8  version, type, channel /* 1..8; 0 = none */, flags;
	u32 sender;      /* stable per console: the low 4 bytes of the MAC address */
	u16 seq;         /* per sender, per type */
	u16 samples;     /* AUDIO: sample frames that follow (160) */
	u32 timestamp;   /* AUDIO: the sender's sample clock at 16 kHz */
};
/* HELLO payload: char name[16] (gamertag of the pad's user, or "XBOX-xxxx"), u8 level, u8 reserved[3] */
/* AUDIO payload: samples x s16, 16 kHz mono */
```

**Discovery and teams.**
- Every console with team voice on broadcasts a HELLO once a second on its channel, and a BYE when it leaves. A peer is dropped after 5 s of silence. There is no pairing and no server.
- A player picks a channel on the panel: row `TEAM CH` = OFF, 1..8. Players on the same number hear each other. The row shows the number of consoles heard on that channel (`team_peers`), so a team can see they are all on.
- Row `TEAM VOL` 0..10 is the third level. The mix becomes: earpiece = master x (game x game_level + (chat + team x team_level) x chat_level) + sidetone. Balance still trades game against voices.
- Talkers: up to 8 peers tracked per channel; the 4 most recently active are mixed. A 4v4 team has 3 others, so nothing is dropped in practice.

**Audio.**
- Codec: none. 16 kHz mono s16, 10 ms packets (160 samples, 340 bytes with the header, 100 packets/s while talking).
- Send side: the mic after mic level and the noise gate (the same signal the 360 gets), resampled from the headset rate to 16 kHz with the existing integer resamplers in `voice_360.c`. Sent only while the gate is open, plus a 200 ms hang. Muted = not sent.
- Receive side: one jitter buffer per talker (ring of 16 kHz samples; 30 ms target, 20-80 ms adaptive). Reorder by `seq`; conceal a lost packet by repeating the last one faded; reset on a gap over 200 ms.
- Clock drift (the sender is paced by its pad, the receiver by its own): the fill-steered resampler pattern from `game_360.c`, per talker, 16 kHz to the earpiece rate.
- Latency budget: 8 ms mic block + 10 ms packet + 30 ms jitter + 40 ms OUT queue, about 90 ms. The OUT queue is the same 40 ms that delays sidetone; if WP1.6 lands, its late-mix can serve team voice too.

**In-game voice.** With a team channel on, teammates would hear each other twice (Halo 3's own voice plus ours, at different delays). So:
- Panel row `MIC TO`: `TEAM` (default when a channel is on), `GAME`, `BOTH`. On `TEAM`, the 360's capture packets get silence (the existing mute path), so the game sends nothing for this player. Incoming game chat is still played, so players outside the system are still heard.
- `BOTH` is for mixed lobbies and accepts the doubling.
- Mute (Share tap) silences both.

**Echo and feedback.**
- No acoustic echo canceller. Closed headsets leak little into the mic, and the gate (default low) closes between words. The feedback guard stays on sidetone only.
- Same-room double hearing (the player next to you, live and then about 90 ms later in the headset) is inherent to any digital daisy chain. `TEAM VOL` is the user's control.
- Nothing that was received is ever sent back out, so there is no network echo path by construction. A packet whose `sender` is our own id is dropped (broadcast loopback).

**Threads.**
- A new system thread (`XtNetThread`, NV64 thunk, not on hardware thread 2 or 4, priority below the driver thread) owns the socket: non-blocking, `select` with a 5 ms timeout.
- The driver thread never calls NetDll. Two lock-free single-producer rings connect them: mic out (driver to net) and per-talker PCM in (net to driver).
- Hot reload: `xt_quiesce` sends BYE, closes the socket and waits for the thread to end, with a refusal code if it doesn't.

### 4.2 Contract: `team_360.h`

```c
/* driver thread unless noted */
void xt_init(void);                 /* nothing opened: gated by XWTV.stage */
void xt_poll(void);                 /* stage changes, peer ageing, status */
void xt_quiesce(void);  void xt_resume(void);
/* voice_360.c -> team: the mic as the 360 gets it (after level and gate), s16le at rate x ch;
 * `open` = the gate is passing it. Called per mic packet. */
void xt_mic(int pad, const u8 *pcm, int len, u32 rate, u32 ch, bool open);
/* voice_360.c's xv_earpiece_fill -> team: adds the team mix to an earpiece block, at `gain`
 * (Q8: master x chat_level x team_level), clamped; like xg_mix */
void xt_mix(int pad, u8 *out, int len, u32 rate_out, u32 ch_out, u32 gain);
/* panel */
u32  xt_channel(void);   void xt_set_channel(u32 ch);     /* 0 off, 1..8 */
u32  xt_level(void);     void xt_set_level(u32 level);    /* 0..10 */
u32  xt_mic_route(void); void xt_set_mic_route(u32 r);    /* 0 TEAM, 1 GAME, 2 BOTH */
u32  xt_peers(void);     u32 xt_talking_mask(void);
bool xt_mic_to_game(void);          /* voice_360.c: false = give the 360 silence */
```

xbdm block `XWTV`:
- controls: `stage` (0 off, 1 socket only, 2 hello, 3 receive, 4 send), `channel`, `level`, `mic_route`, `test_tone` (send 440 Hz instead of the mic);
- state: socket errors by call, `tx_hello/audio/bytes`, `rx_hello/audio/bad/own`, and per peer `id, name, age_ms, rx, lost, late, jitter_ms, fill_ms, underruns, trim_ppm`.

`net_360.h` wraps the NetDll calls (`xn_open_udp(port)`, `xn_send_broadcast`, `xn_recv`, `xn_close`, each returning the raw error), so `team_360.c` has no ordinals in it.

### 4.3 Work packages

**WP4.0 Network spike (one engineer; can start any time; needs the console for about an hour).** Files: `net_360.c/.h`, `team_360.c` (stage 1-2 only), `team.py`.
- Verify the ordinals against the XAM dump. Fetch HaloLAN.xex and JRPC2.xex from the console and list their NetDll imports and caller types, as existence proof and for later integration.
- On the console, gated: open the socket as the system caller; send a HELLO broadcast once a second; receive.
- `team.py` on the Mac is the second peer: `team.py listen` prints HELLOs, `team.py hello` sends them, `team.py tone CH` sends a 440 Hz AUDIO stream, `team.py record CH FILE.wav` records a channel.
- Answer, with counters:
  - does it need `XNetStartup` or `WSAStartup` first?
  - is 0x5801 needed, and does it work?
  - do broadcast send and receive both work?
  - does the socket survive Aurora to Halo 3 and back, and Halo 3 entering and leaving a system-link lobby?
  - does Halo 3's own networking still work with our socket open?
  - what is the cost per `sendto`?
- Acceptance: `accept/p4_net.py`: 60 s of HELLOs both ways with zero socket errors, across two title switches.

**WP4.1 Receive path (engineer G; after 4.0).** `team_360.c`: peer table, jitter buffers, drift resampler, `xt_mix`; the one-line call in `voice_360.c`'s earpiece fill (coordinate with D; it sits next to `xg_mix`). Acceptance: `team.py tone 1` from the Mac for 5 minutes: `lost` 0, `underruns` 0 after the first second, `fill_ms` within 20-50, trim within +/- 500 ppm; the earpiece `dump` contains 440 Hz at the expected level; `out_starved` +0. the owner check: he hears a clean tone under game audio, and `TEAM VOL` changes it.

**WP4.2 Send path (engineer H; parallel with 4.1; after 4.0).** `team_360.c` send half, `xt_mic` call in `voice_360.c` after the gate, `xt_mic_to_game` in the capture path. Acceptance: `team.py record 1 out.wav` while the owner talks: speech is clean, packets stop within 250 ms of silence (`tx_audio` flat), muted sends nothing, `MIC TO TEAM` makes the 360's capture silent (`voice.py status` capture peak 0) and `BOTH` does not.

**WP4.3 Panel rows (engineer C; after 4.1/4.2 land their getters).** `panel_360.c`, `hud_panel.c`: rows `TEAM CH`, `TEAM VOL`, `MIC TO`, shown only when the team stage is on; `team_peers` and talking dots. Acceptance: by `inject`, as in WP1.3, plus screenshots.

**WP4.4 Two consoles.** Needs a second console running the same build; the owner has them at the LAN, and whether one is on the dev network is his call. Until then the Mac peer stands in for every test above. Acceptance: two consoles on channel 1 in a Halo 3 system-link game, both ways, 30 minutes; a third on channel 2 hears nothing.

### 4.4 Later work enabled by this design (not built now)

- **PC peer app** (OBS capture, admin talk-back): `team.py` grown into a small app. Because audio is broadcast, it listens to any or all channels with no change on the consoles and exposes each channel as an audio device or a WAV/ndjson stream for OBS. Talk-back is an AUDIO stream with `XT_F_PC`, and `XT_F_ALLCALL` to reach every channel.
- **Phones and other devices:** the same protocol over Wi-Fi on the same subnet, or the PC app as a bridge (WebRTC to the browser).
- **HaloLAN integration:** HaloLAN knows the teams; it sets `XWTV.channel` (find the block by magic in the xw module, or an exported setter). Nothing in Phase 4 depends on it.

### 4.5 Pros, cons, risks

Pros:
- no PC or server at the LAN;
- no pairing;
- plain PCM, with no codec to port to a no-FP kernel module;
- everything is observable and testable from the Mac over the same protocol;
- OBS and admin listening come free.

Cons:
- about 90 ms latency against a real daisy chain's 0;
- no encryption or authentication: anyone on the LAN can listen or talk. Acceptable for a private LAN; say so.
- broadcast does not cross subnets or most Wi-Fi isolation;
- one headset per console.

Risks:

| Risk | Mitigation |
|---|---|
| The system-caller socket model does not behave as Xenia's sources suggest on 17559 | WP4.0 answers it before any audio code is written |
| Our socket disturbs Halo 3's networking (XNet state, port or socket limits) | WP4.0 tests a system-link lobby; fallback is a title-caller socket re-opened per title |
| NetDll calls block or are slow | Own thread, non-blocking, rings to the driver thread |
| More CPU on the driver thread (4 resamplers) brings the ticks back | Do the resampling on the net thread; the driver thread only adds samples; gate on `out_starved` |
| Doubling with in-game voice | `MIC TO` default TEAM |
| Hot reload with a socket open | `xt_quiesce`; a refusal code if the thread doesn't end |

### Phase 4 definition of done

Two consoles on the same channel talk both ways in a Halo 3 system-link match for 30 minutes with `lost` under 0.1 %, no underruns and `out_starved` 0. A console on another channel hears nothing. `TEAM CH`, `TEAM VOL` and `MIC TO` work from the panel. The Mac peer records a channel. Until a second console is available, the same with the Mac peer standing in, stated as such.

---

## Later (list only)

- Virtual surround: 5.1 to binaural from the MEC tap (the tap already has all six channels; integer HRIR filters).
- Two or more headsets per console. The 12-iTD pool is shared by all OHCI ports and one headset uses 10. Options: fewer blocks per headset, or one mixed OUT stream.
- Other pads (PowerA, PDP, 8BitDo): `pad_quirks.h` entries and start-up quirks.
- HaloLAN integration: automatic team channels.
- Settings saved to a file on the HDD.
- Waking a dark pad after a cold boot without the button press (for example root-hub port power cycling, if the 360's OHCI ports support it). This would remove the main cost of a reboot.
- Status notices behind an "advanced" setting (from CLAUDE.md).

---

## Ordering, dependencies, parallelism

```
WP0.1 integration ──┬─> WP1.1 HUD bring-up (A) ───────────────┐
WP0.2 tooling ──────┤   WP1.2 panel renderer (B) ─────────────┼─> Phase 1 done
(parallel)          ├─> WP1.3 controls + menu removal (C) ────┤
                    ├─> WP1.4 audio defaults/cleanup (D) ─> WP1.5 ─> (WP1.6)
                    ├─> WP3.0 pre-flight ─> WP3.1 (F) ─> WP3.2 ─> WP3.3 ─> WP3.4
                    └─> WP4.0 net spike (G) ─> WP4.1 (G) ∥ WP4.2 (H) ─> WP4.3 (C) ─> WP4.4
WP1.3 ─> WP2.1 SCUF (C or E)
```

- After WP0.1, six engineers can work without sharing a file: A (`hud_360.c`), B (`hud_panel.c`), C (`panel_360.c`, `xinput_360.c`), D (audio files), F (reload region, `handover.h`), G (`net_360.c`, `team_360.c`).
- The only shared-file touch points are pre-wired by WP0.1, plus two one-line calls in `voice_360.c` for Phase 4, which go through D.
- Console time is serial. Suggested order of sessions:
  1. WP0.1 acceptance (one reboot);
  2. WP3.0 pre-flight (no build);
  3. WP3.2 E1-E4, timeboxed to one session, because a working reload makes every later session cheaper;
  4. WP1.1 H0-H8;
  5. WP1.3, WP1.4, WP1.2 acceptance;
  6. WP1.5;
  7. WP2.1;
  8. WP4.0;
  9. the rest of WP3 and WP4.
- If hot reload is not fixed within its timebox, Phase 1 proceeds on reboots. Batch several packages into one build per reboot.

## Cross-phase risks

| Risk | Mitigation |
|---|---|
| A freeze with no evidence | `xw.py tee` on every session; trace words; staged gates; every new hook and USB or network feature off by default until accepted |
| Hot reload plus new hooks (VdSwap) and new threads (net, maybe draw) multiplies the teardown cases | Each new module ships its `*_quiesce/_resume` and a row in HOT-RELOAD.md's table with it; WP3's acceptance loop runs with everything on |
| The driver thread (hardware thread 2) gains work in every phase | A standing budget check in every acceptance script: `out_starved` +0, `mic_pps` 1000, driver loop max period (add a `loop_max_us` counter in WP0.1) |
| Kernel or XAM addresses are 17559-only | Every use is behind a signature check that refuses; unchanged policy |
| Another plugin (HaloLAN, JRPC2) hooks the same functions | WP3.0's `xw.py hooks`; `detour_install` refuses foreign patches (WP3.3) |
| Status-block layout drift between C and Python | Version fields and asserts; tools refuse unknown versions |

### Critical Files for Implementation
- /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360/xw_plugin.c
- /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360/hud_360.h (and `hud_360.c` from commit 4266a69 on branch `hs-hud`)
- /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360/voice_360.c
- /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360/detour.c
- /Users/owner/Documents/code/Xbox360-GIP-Wired/xbox360/xw.py (with `handover.h` for Phase 3)
