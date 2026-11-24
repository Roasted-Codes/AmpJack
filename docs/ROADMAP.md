# ROADMAP: the next block of work on xw.xex

Written Nov 23 2026 by the planning agent, for an orchestrator driving up to 5 coding workers.
**Status: approved by the owner on Nov 23 2026 (night), with the decisions in Part 3, which win over Parts 1 and 2.** Part 1 is the plan;
Part 2 is an addendum with his later scope changes and **wins where the two disagree** (AUTO is
an added value, the wireless investigation is replaced by breadcrumbs, and a code-quality
track is added). Base for all of it: `main` with the branches `notify-fix` and `auto-quiet`
merged.

---

# Part 1: the plan

Repo: `/Users/owner/Documents/code/AmpJack`, read on `auto-quiet` (HEAD 1484ad3). Nothing was modified and the console was not touched. Everything below is from reading the code; nothing was measured. CPU estimates are mine and unverified.

## 0. Facts that shape the plan

| Fact | Where | Consequence |
|---|---|---|
| Image is 0xde000, and `.data` alone is 0x99400 (613 KB), because zero-initialised statics live in `.data`. | `xbox360/out/xw.exe` section table | Every new static buffer costs image bytes. New modules malloc any state over about 1 KB. Reclaimable if needed: `rx_ring` 128 KB (`platform_360.c:208`), the two HUD canvases 59 KB (`hud_360.c:508,517`), `g_ring` 32 KB (`game_360.c:159`). |
| `voice_360.c` is already in clean regions: XVoiced core 1-1365, sidetone / headset volume 1366-1946, XWDG + mic DSP 1947-2254, mic test 2255-2592, timing 2593-2772, interface 2773-3019. | `voice_360.c` | A mechanical split is cheap. |
| Shared touch points are three functions: `xv_mic_samples` (2879), `xv_earpiece_fill` (2915), `xd_poll` (2773). | `voice_360.c` | Pre-wire call sites once; features never edit them. |
| `hud.py show` writes u32 `test_*` words in XH36, not the struct. The "24 bytes" assert message is historical. | `hud_360.c:143-281`, `hud.py:134` | `struct hud_state` can grow freely. |
| The panel draws rows in enum order, and `cursor_move` walks enum order. | `hud_panel.c:359`, `panel_360.c:323` | A shared display-order table is needed to put PRESET / SURROUND above TEST TONE. |
| xw.xex has no file I/O. `xa_plugin.c` is not in `XW_SRC` and only reads. `xa_kcall9` is available (`thunks_common.s`, included by `thunks_wired.s`). | `build.sh`, `xa_plugin.c:487` | File read/write is new code. |
| The tap runs on the kernel render thread: priority 25, hardware thread 4, one call per 256 frames (5.33 ms), before the buffer goes to the hardware. | `game_360.c:5-13`, `research/AUDIO.md:459-522` | Work in the tap delays TV audio. XWDG `work_us` / `pass_us` time the driver thread only, so a tap load is invisible to them. |
| The driver thread is on hardware thread 2 at priority 9 and loses the CPU for about 15.6 ms at a time. The HUD draw thread is on hardware thread 1 at priority 15 and samples every 2 ms while something is visible. | `diag_360.h:258-263`, `hud_360.c` v6-v9 comments | Affects where surround runs, and the cost of a permanent mute mark. |
| Halo 3's front buffer is 1152x640 (the "640p" case). | `research/HUD.md:253-259` | Used for the panel fit check below. |
| `XW_SRC` already links `core/mt76.c` and `core/dongle.c` into xw.xex. | `build.sh` (`$XW_SRC $CORE`) | Relevant to "wired + wireless in one plugin". |

## 1. Waves and dependency graph

```
W0  BASE-1 contracts (serial, small) ---------------------------------+
    in parallel, new files only: WIRELESS finding | EQ offline         |
    | HRTF offline | MIC-AUTO offline                                  |
                                                                       v
W1  BASE-2 split + pre-wire | PANEL (all rows + mute mark) | EQ plugin
    | SURROUND (probe, then engine) | SETTINGS (probe, then feature)
        --> console S1: base regression + all probes
W2  MICTEST v2 | MIC-AUTO plugin | SURROUND engine | SETTINGS finish | fixes
        --> console S2: batch A
        --> console S3: batch B (surround, leftovers)
        --> console S4: release candidate, no computer
```

| Wave | Worker 1 | Worker 2 | Worker 3 | Worker 4 | Worker 5 |
|---|---|---|---|---|---|
| W0 "Contracts" | BASE-1 | WIRELESS finding | EQ offline (parser, converter, reference model) | HRTF offline (dataset, generator, reference model) | MIC-AUTO offline (gate / AGC / FIR on the recorded mic WAVs) |
| W1 "Foundations" | BASE-2 | PANEL | EQ plugin | SURROUND probe, then engine | SETTINGS probe, then feature |
| W2 "Features" | MICTEST v2 | MIC-AUTO plugin | SURROUND engine (after S1 numbers) | SETTINGS finish | PANEL fixes / integration helper |

Serial points:
- BASE-1 must merge to a shared base before any W1 work starts. It is headers, stubs and `build.sh` only; it needs no console run on its own and is verified with BASE-2 in S1.
- MICTEST and MIC-AUTO plugin work waits for BASE-2 to be code-complete (they live in the split files). They may start before S1 and accept a rebase if S1 forces fixes.
- The SURROUND engine design is chosen from S1's numbers.
- The orchestrator alone edits shared lists: `build.sh` `XW_SRC`, `xw.py` `tee` / `knob_specs`, `CLAUDE.md`, `docs/HANDOFF.md`.

Git:
- One branch per package: `base-contracts`, `base-split`, `panel-v4`, `eq-presets`, `surround`, `settings`, `mictest-v2`, `mic-auto`, `wireless-finding`. All pushed to `private` only.
- W0 offline work goes on the feature's own branch (new files only).
- For a console session the orchestrator builds a throwaway `batch/Sn` branch (main plus the candidate branches). It is never merged and is deleted after the session.
- A feature that passes is merged to `main` on its own and its branch deleted. A failed one stays out and goes to the next batch.
- Exception to flag to the owner: W1 packages are built on `base-contracts` before it has been console-verified. That is unavoidable for parallelism; BASE is verified first thing in S1.

Rules for every package:
- Each feature has its own xbdm gate, default off until its acceptance passes; the flip is a separate one-line commit.
- Each feature has its own new status block with a new magic and its own new Python module. Do not append to XWDG, XVGA or XVHS; that is where collisions come from.
- Each feature ships `*_quiesce` / `*_resume` so hot reload does not get harder.
- No FP, no .bss, `check_xex.py` passes.
- The worker reports the image size before and after.

## 2. Contracts to freeze in BASE-1

### 2.1 `hud_360.h` version 4 (56 bytes; the first 24 keep their positions)

```c
enum { HUD_HIDDEN, HUD_TOAST, HUD_PANEL, HUD_BADGE };   /* BADGE: only the small mute mark */
enum { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE, HUD_ROW_MIC, HUD_ROW_GATE, HUD_ROW_TONE,
       HUD_ROW_TEAM, HUD_ROW_CHANNEL, HUD_ROW_MICTEST, HUD_ROW_PRESET, HUD_ROW_SURROUND, HUD_ROWS }; /* 11 */
/* the order rows are drawn and the cursor moves in (renderer AND panel_360.c use this) */
static const u8 hud_row_order[HUD_ROWS] = { HUD_ROW_BALANCE, HUD_ROW_VOLUME, HUD_ROW_SIDETONE,
    HUD_ROW_MIC, HUD_ROW_GATE, HUD_ROW_PRESET, HUD_ROW_SURROUND, HUD_ROW_TEAM, HUD_ROW_CHANNEL,
    HUD_ROW_TONE, HUD_ROW_MICTEST };
enum { HUD_MT_IDLE, HUD_MT_RECORDING, HUD_MT_PLAYING, HUD_MT_READY };  /* READY = a clip is held */
enum { HUD_GATE_OFF, HUD_GATE_LOW, HUD_GATE_HIGH, HUD_GATE_AUTO };
#define HUD_NAME_LEN 20

struct hud_state {
    /* [0..22] as version 3, with these meaning changes: */
    /*   mic_level    0 = AUTO, 1..10                                              */
    /*   gate         HUD_GATE_*                                                   */
    /*   mictest      HUD_MT_* (adds READY)                                        */
    /*   mictest_secs RECORDING: seconds recorded so far (count-up);               */
    /*                PLAYING / READY: the clip's length in seconds                */
    u8 mic_auto_level;  /* [23] was reserved: MIC VOLUME AUTO's level now, 1..10 (0 unknown) */
    u8 preset;          /* [24] 0 FLAT, 1..preset_count */
    u8 preset_count;    /* [25] 0 = no preset file (row shows FLAT only) */
    u8 surround;        /* [26] 0 OFF, 1 ON */
    u8 surround_live;   /* [27] 1 = centre/surround channels carry sound now (the dot) */
    u8 badge;           /* [28] HUD_BADGE: bit 0 = mic muted (other bits free) */
    u8 badge_shift;     /* [29] 0..3, moves the mark a unit now and then (burn-in) */
    u8 mictest_max;     /* [30] auto-stop seconds (30, or 10 after an allocation fallback) */
    u8 reserved0;       /* [31] */
    char preset_name[HUD_NAME_LEN]; /* [32..51] NUL padded, panel font characters only */
    u8 reserved[4];     /* [52..55] */
};
_Static_assert(sizeof(struct hud_state) == 56, "hud_360.h v4");
```
- `rows` / `rows_hi` are unchanged (PRESET = `rows_hi` bit 1, SURROUND = bit 2).
- XH36 goes to version 11 by appending `test_mic_auto_level, test_preset, test_preset_count, test_surround, test_surround_live, test_badge, test_badge_shift, test_mictest_max, test_preset_name[5]` and one control `badge_pos`. `hud.py` `VERSIONS` and `show` follow.

### 2.2 `hud_panel.h`
- `HP_H_MAX` 144 (all 11 rows).
- `struct hp_canvas` gains `u8 anchor` (`HP_ANCHOR_BOTTOM_CENTRE` for panel and toast, `HP_ANCHOR_BADGE` for the mark). `hud_360.c`'s `geometry()` honours it.
- PRESET row: the name is drawn across the control and value columns, x 80..215, about 22 characters.
- BASE-2 should malloc the two canvases instead of growing the statics (saves about 59 KB; otherwise this costs about 10 KB).

### 2.3 Feature getters / setters (BASE-1 ships stubs that return the defaults)

```c
/* diag_360.h -> implemented in micdsp_360.c (MIC-AUTO) */
enum { XD_GATE_OFF, XD_GATE_LOW, XD_GATE_HIGH, XD_GATE_AUTO };  /* g_xd.gate; XD_GATES 4; default AUTO after acceptance */
bool xd_mic_auto(void);   void xd_set_mic_auto(bool on);   /* MIC VOLUME AUTO; g_xd.mic_gain stays the manual value */
u32  xd_mic_auto_level(void);                              /* 1..10 in effect now */

/* diag_360.h -> mictest_360.c (MICTEST) */
#define XD_MT_MAX_MS 30000
enum { XD_MT_IDLE, XD_MT_RECORDING, XD_MT_PLAYING, XD_MT_READY };
u32  xd_mictest_start(int pad);            /* IDLE or READY -> RECORDING (a held clip is dropped) */
void xd_mictest_stop(void);                /* RECORDING -> PLAYING (>= 500 ms recorded), then READY */
u32  xd_mictest_play(void);                /* READY -> PLAYING -> READY */
void xd_mictest_cancel(const char *why);   /* RECORDING: discard -> IDLE; PLAYING: fade -> READY; READY: discard -> IDLE */
u32  xd_mictest_state(u32 *secs, u32 *pct);
u32  xd_mictest_max_secs(void);

/* eq_360.h (new) -> eq_360.c (EQ) */
void xq_init(void);  void xq_poll(void);  void xq_quiesce(void);
void xq_process(int pad, u8 *out, int len, u32 rate_out, u32 ch_out); /* in place; no-op on FLAT or rate != 48000 */
u32  xq_count(void);  u32 xq_preset(void);  void xq_set_preset(u32 idx);  /* 0 = FLAT */
u32  xq_preset_id(u32 idx);  u32 xq_find(u32 id);  /* stable id (FNV-1a of the name); 0 = FLAT */
const char *xq_name(u32 idx);

/* game_360.h -> game_360.c + surround_360.c (SURROUND) */
u32  xg_surround(void);  void xg_set_surround(u32 on);  bool xg_surround_live(void);

/* file_360.h (new) -> file_360.c (SETTINGS owns it; EQ only calls it) */
int  xf_read(const char *path, void *buf, u32 max, u32 *len);   /* never on the driver thread */
int  xf_write(const char *path, const void *buf, u32 len);
bool xf_post(void (*fn)(void *), void *arg);   /* run fn on the file worker thread; false = busy */

/* settings_360.h (new) -> settings_360.c (SETTINGS) */
void xs_init(void);  void xs_poll(void);  void xs_quiesce(void);  /* quiesce = flush now */

/* voice_priv.h (new, BASE-2): what the split files share */
/* g_pad[], hs_format_ok, struct rs / rs_init / rs_push, clamp16, isqrt, isin, g_mic_mute, */
/* struct xd_shape / shape(), mic_gain_target() (returns the gain in effect, AUTO included), */
/* void xv_cap_feed(int pad, const s16 *s, int n);  -- the 360-rate capture path, for the new decimator */
```

### 2.4 Audio order (frozen)

Earpiece:
- Render thread (tap, 48 kHz): float to Q15 on 6 channels, then either the virtualizer (SURROUND on) or today's downmix, into the existing stereo ring.
- Driver thread (`xv_earpiece_fill`, 48 kHz stereo): chat x chat gain, + game (`xg_mix`), + team (`xt_mix`), + test tone, + software sidetone, + centre tick, + mic-test playback, then **`xq_process` (EQ over the whole mix)**, then a reserved slot for the later limiter / night mode, then the existing "anything to play" scan (`dev_quiet`), then USB and the controller's amplifier (`dev_vol`).

Mic (24 kHz mono, in `xv_mic_samples`):
- `mic_clean` (DC), then an optional comb canceller (off by default), then `mic_analyse` (20 ms RMS: gate decision, AUTO floor tracker, AGC measurement).
- Consumers as today: `st_mic`, `xt_mic`, `mt_record`.
- Then `shape()` (manual or AUTO gain, gate, limiter), then the **new FIR decimator 24 to 8 kHz**, then `xv_cap_feed`.
- For other rate ratios `rs_push` stays.

### 2.5 Settings file `Hdd:\Plugins\xw.settings` (big-endian)

```
header 16 B: 'XWST', u16 version = 1, u16 rec_size = 48, u16 count, u16 capacity = 64, u32 reserved
record 48 B:
  u64 xuid            0 = "nobody signed in" record; all FF = empty slot
  u32 seq             bumped on every save; lowest is evicted when full
  u8  ver(=1), s8 balance, u8 volume(0..10), u8 sidetone(0..10)
  u8  mic_level(0 AUTO, 1..10), u8 gate(XD_GATE_*), u8 surround, u8 flags
  u32 preset_id       xq_preset_id; 0 FLAT; an unknown id loads as FLAT
  u8  team_level, team_channel, mic_route, reserved[17] (zero)
  u32 crc32 of the 44 bytes before it
```
- Not saved: mute, test tone, mic test, engineering knobs (`buffer_ms`, `dev_*`).
- The whole file is under 3.1 KB: loaded once into RAM and rewritten whole on a save.

### 2.6 Preset file `Hdd:\Plugins\xw.presets` (big-endian)

```
header 32 B: 'XWPQ', u32 version = 1, u32 rate = 48000, u32 count, u32 rec_size = 160, u32 bands = 5,
             u32 crc32 (everything after the header), u32 reserved
record 160 B: u32 id; char name[20]; char author[16]; s32 preamp_q15 (<= 32768); u32 flags;
              s32 coef[5][5]  /* b0 b1 b2 a1 a2, Q3.28, a0 = 1 */; s16 gain_cdb[5]; u8 pad[2]
```
- 29 presets (the TheZEFERENCE count today) come to about 4.7 KB.
- Biquads are direct form 1, s16 in, 64-bit accumulate, per channel.
- The generator proves that peak gain after the preamp is 0 dB or less.

### 2.7 HRTF table
- Recommendation: a generated header, not a file. The table is about 0.7 KB at 64 taps (1.3 KB at 128), so a missing file cannot break surround.

```
struct xsr_table { u32 magic 'XWSR'; u16 version, taps (<= 128, multiple of 4); u32 rate (48000);
                   s16 lfe_q14, out_q14; u32 crc; s16 h[5][taps]; };
/* h: front-sum, front-diff, surround-sum, surround-diff, centre; Q14; sum|h| <= 2.0 per filter */
```
- If the owner prefers a file, the same bytes go to `Hdd:\Plugins\xw.hrtf` via `xf_read`.

### 2.8 New status blocks (one per package, own Python module)

| Block | File | Tool |
|---|---|---|
| `XWMA` mic auto | `micdsp_360.c` | `micauto.py` |
| `XWMT` v2 (appended) | `mictest_360.c` | `voice.py` mictest part |
| `XWEQ` | `eq_360.c` | `eq.py` |
| `XWSR` | `surround_360.c` | `surround.py` |
| `XWST` | `settings_360.c` | `settings.py` |
| `XWPN` v3 (appended) | `panel_360.c` | `accept/wp1_3.py` |
| `XH36` v11 | `hud_360.c` | `hud.py` |

`voice.py` is moved wholesale to MICTEST for this block; nobody else edits it.

## 3. Work packages

### BASE-1 Contracts (S), branch `base-contracts`
- Goal: freeze section 2; everything compiles and behaves exactly as today.
- Owns: `hud_360.h`, `hud_panel.h` (constants only), `diag_360.h`, `game_360.h`, `build.sh`; new `eq_360.h`, `file_360.h`, `settings_360.h`; stub `eq_360.c`, `surround_360.c`, `settings_360.c`, `file_360.c`.
- Minimal compile fixes only in `hud_panel.c`, `panel_360.c`, `hud_360.c` (sizes and asserts).
- Off-console: toolchain build prints `ok, no FP`; image size delta reported; `tests/test_team*.c` still build.
- Risk: a contract mistake found later costs every worker. The orchestrator reviews section 2 before merging.

### BASE-2 Split, pre-wire, harness, installer (M), branch `base-split`
- Goal: no behaviour change.
- Split `voice_360.c` mechanically into `voice_360.c` (XVoiced core + `xv_*` interface), `side_360.c`, `micdsp_360.c` (XWDG, tone, mic clean / analyse / shape, timing), `mictest_360.c`, with `voice_priv.h`.
- Pre-wire the call sites: `xq_process` in `xv_earpiece_fill`; `xv_cap_feed` and decimator hook in `mic_shaped`; `xs_poll` / `xq_poll` in the main loop (`xw_plugin.c:3468-3472`); `xs_quiesce` / `xq_quiesce` in `do_release`.
- Malloc the HUD canvases.
- New `tests/test_panel.c`: `hp_build` to PPM for a list of states (worker self-check only; never shown to the owner).
- New `xw.py install`: sends `xw.xex`, `xw.presets`, keeps `.prev`, verifies, `magicboot cold`. Replaces the inline snippet in HANDOFF section 2.
- Owns: the four split files, `voice_priv.h`, `xw_plugin.c`, `hud_360.c` (canvas allocation only, then handed to PANEL), `tests/test_panel.c`, `xw.py`.
- Off-console: build; symbol-list diff before and after (same functions, none lost); host panel test renders v3 states identically to before.
- On-console (S1): `accept/run.py wp1_3 wp1_4 mictest sidetone p1_panel` all PASS as before. Owner: "does everything sound and look the same as yesterday?"
- Risk: the move changes inlining and so timing. Watch `work_max_us`, `out_starved`.
- Fallback if the orchestrator does not want the split: keep one file and assign regions by function prefix (`mt_*` to MICTEST, `mic_*` / `xd_*` to MIC-AUTO). The pre-wired call sites are still required.

### PANEL All new rows and the mute mark (M-L), branch `panel-v4`
- Goal: PRESET and SURROUND rows; MIC VOLUME with AUTO left of 1; NOISE GATE as OFF / AUTO; MIC TEST states (READY, count-up, replay); display order from `hud_row_order`; the persistent mute mark.
- Owns: `hud_panel.c`, `hud_panel.h`, `panel_360.c`, `panel_360.h`, `hud_360.c` (after BASE-2's canvas change: anchor, `HUD_BADGE`, XH36 v11), `hud.py`, `accept/p1_panel.py`, `accept/wp1_3.py`, `tests/test_panel.c`.
- Depends on section 2.1-2.3 only (works against the stubs).
- Row logic:
  - MIC VOLUME: Left from 1 goes to AUTO.
  - NOISE GATE: Left = OFF, Right = AUTO (LOW / HIGH stay reachable over xbdm).
  - PRESET: Left / Right with hold-to-repeat (about 8 a second after 400 ms; there are 29 presets), clamped.
  - MIC TEST Right: IDLE = record, RECORDING = stop and play, READY = play. Left: discard while recording, stop while playing, discard when READY.
  - The mic-test toast shows only while recording or playing, not for READY.
- Mute mark:
  - `vis = panel ? PANEL : (toast or test running) ? TOAST : muted && XWPN.badge ? BADGE : HIDDEN`.
  - A dark-red dot plus dim "MUTED", about 46x11 units (92x22 px at 720p), no background box (transparent canvas), moved 1 unit every 60 s.
  - Default position: bottom centre in the band between the 10 % and 4 % margins (below where the panel sits, so nothing jumps). `XH36.badge_pos` selects other corners over xbdm, so the owner can choose in S2 without a rebuild.
- Off-console: `tests/test_panel.c` renders every state at 11 rows; asserts height 144 and no text overlap; XWPN inject logic in a host harness if cheap.
- On-console: `accept/p1_panel.py` (extended to the new states), `accept/wp1_3.py` (inject drives every new row), new `accept/badge.py` (mute for 10 minutes in Halo 3; `swaps_per_s` unchanged, `pres_pct` >= 95, draw time < 0.25 ms).
- Owner: "Tap Share. Is the small red MUTED mark there, steady, and not in your way? Can you read all rows?"
- Risks: the mark may shimmer (the title redraws the buffer and we repaint within 2 ms). The draw thread then runs all the time on hardware thread 1, which upstream's PR #7 says is the title's main thread. Fallbacks: raise `idle_ms` for the mark only, or show it for 2 s out of every 15.

### MICTEST v2 (M), branch `mictest-v2`
- Goal: HANDOFF 4.1 with the section 2.3 API. The buffer is 1.44 MB from `ExAllocatePool` on first use; fall back to 10 s if it fails (`mictest_max` tells the panel).
- Owns: `mictest_360.c`, `voice.py`, `accept/mictest.py`.
- Publishes XWMT v2 (appended): `clip_ms`, `max_ms`, `plays`, `discards`, `fallback`. `rec_ms` becomes the auto-stop, default 30000.
- Off-console: host test `tests/test_mictest.c` (state machine on a fake clock: every Right / Left transition, auto-stop, mute during record, pad leaving in each state).
- On-console: `accept/mictest.py` (rewritten; all states over xbdm inject; allocation with Halo 3 running).
- Owner: "Record yourself for as long as you like, press Right to stop, hear it, press Right to hear it again, Left to clear."
- Risk: 1.44 MB of kernel pool may not be available under a game. S1 probes it. Fallback: 10 s.

### MIC-AUTO Gate AUTO, MIC VOLUME AUTO, anti-alias filter (M-L), branch `mic-auto`
- Goal: HANDOFF 4.3.
  - Gate AUTO: minimum-statistics floor over about 3 s of 20 ms windows; threshold = floor + 8 dB, clamped -60..-35 dBFS; existing 200 ms hold and 10 ms ramps.
  - AGC: target -30 dBFS RMS on gate-open windows, range 25..560 %, 1 dB/s, frozen while shut.
  - FIR low-pass and decimate by 3 (about 45 taps, Q15, integer).
  - Comb canceller behind a control, default off.
- Owns: `micdsp_360.c`, new `micauto.py`, `tools/micdsp/` (design and reference model), `tests/test_micdsp.c`, `accept/micauto.py`, the gate / mic-gain part of `xw.py diag`.
- Publishes XWMA: controls `mic_auto, fir, comb`; state `floor_rms, thr_rms, agc_gain_q12, agc_level, speech_rms, frozen`. `g_xd.gate` accepts 3 (XD_VERSION 4, same size).
- Off-console (the bulk of the work):
  - Run the integer reference model over the real recordings in `research/results/2026-11-23-buzz/` and `xbox360/out/mictest.wav`: gate opens on every word, stays shut between; AGC settles within 5 s with no pumping.
  - FIR: passband +/-1 dB to 3.2 kHz, at least 50 dB down above 4.6 kHz, by sweep.
  - The C compiled on the host is bit-exact with the Python model.
- On-console: `accept/micauto.py` (floor and threshold sane with the real headset; AUTO level moves toward target while the owner counts to ten).
- Owner: "Talk normally, then stay quiet: does the gate dot go out when you stop? Record a mic test: does it sound clearer than before?"
- Risks: AGC pumping on breath noise; a wrong floor in a loud LAN room (the clamp bounds it). If shaky, ship gate AUTO + FIR and leave MIC VOLUME manual.

### EQ Equalizer and presets (M), branch `eq-presets`
- Goal: HANDOFF 4.4.
- Owns: `eq_360.c`, `eq_360.h` (after the freeze: comments only), new `eq.py`, `tools/presets/` (parser, converter, shortened-name table, `CREDITS.txt`, the upstream `.astroeq` files), `tests/test_eq.c`, `accept/eq.py`, the built `dist/xw.presets`.
- Preset facts (fetched from the public repo today): 29 `.astroeq` files in 4 folders; no licence file; credit ZaliaS / TheZEFERENCE; file names are 25-55 characters, so the 20-character panel names need a hand-made short-name table.
- First task (W0): settle the band semantics (bands 1 and 5 shelves? bandwidth in octaves?) against Astro Command Center documentation or plots. If it cannot be proven, say so and pick the standard RBJ shelf / peaking reading.
- Loading: at init via `xf_post`. No file means FLAT only.
- Switching: 10 ms crossfade between old and new output.
- Off-console: frequency response of the integer biquads within 0.2 dB of the float design for all 29; no overflow on full-scale noise; host C bit-exact with Python; file round-trip.
- On-console: `accept/eq.py` (load count, select each preset, test tone at 3 band centres measured back through `xw.py dump` if that path sees post-EQ samples, else level counters in XWEQ; `work_max_us` rise < 300 us).
- Owner: "Flip through a few presets during a game: do they sound different, and is none of them distorted?"
- Risks: semantics wrong (presets sound off); presets were tuned for a MixAmp; 29 names on a small row.

### SETTINGS Per-profile save (M), branch `settings`
- Goal: HANDOFF 4.2, with no feature coupling: `xs_poll` snapshots the getters every 250 ms and compares.
- Owns: `settings_360.c`, `file_360.c`, `xinput_360.c` / `.h` (exposes the pad's user index from the bind; nobody else touches these), new `settings.py`, `tests/test_settings.c`, `accept/settings.py`.
- Design:
  - File work never on the driver thread (an HDD stall would starve the earpiece). `file_360.c` runs a small low-priority worker thread; this differs from HANDOFF 4.2's "write from the driver thread".
  - Save 2 s after the last change.
  - Active profile = the XUID on the headset's pad (`xw_headset_pad()`), polled once a second.
  - Restore at boot, headset bind, sign-in change.
  - A profile with no record gets the compiled defaults, not the previous player's values.
  - Nobody signed in uses record XUID 0.
- Unverified: `XamUserGetXUID` / `XamUserGetSigninState` ordinals (I believe 522 / 528). Check the export table in `research/dumps/xam_815f0000.bin`, and whether the call is safe off the XAM thread. S1's probe answers both.
- Off-console: host test with a fake file layer: record round-trip, CRC reject, torn file, 65th profile evicts the oldest, unknown preset id loads as FLAT, version bump.
- On-console: `accept/settings.py` (change three settings over inject, wait 3 s, cold reboot, all three back; file bytes checked over xbdm `getfile`; save latency and audio counters during a save).
- Owner: "Change volume and balance, we restart the console, check they are still where you left them. Then sign in as a second profile and check it has its own."
- Risks: XAM call context; first-write latency; a corrupt file (falls back to defaults, never blocks boot).

### SURROUND (L), branch `surround`
- Goal: HANDOFF 4.5; see section 6.
- Owns: `game_360.c`, `surround_360.c`, new `surround.py`, `tools/hrtf/`, `tests/test_surround.c`, `accept/surround_cost.py`, `accept/surround.py`, the game-audio part of `voice.py status` (read-only; ask MICTEST's owner or the orchestrator for edits).
- Step 1 (W1): probe build for S1. Step 2 (W2): the engine chosen by the numbers.

### WIRELESS Investigation (S-M, no build), branch `wireless-finding`
- Deliverable: `docs/WIRELESS-FINDING.md` (docs only; merges without a console run).
- Questions:
  1. Does the SCUF Instinct Pro pair with the Microsoft adapter at all? It is a licensed Xbox Wireless pad, so it should. Compare `core/dongle.c` / `core/gip.c` paths and descriptor quirks. Is Share in the same report byte over the air (`pad_quirks.h`)?
  2. Headset audio over the link: upstream PR #7 (open, by nobodys-coder, "game audio; microphone untested") does it for the Xbox Wireless Headset 045e:0b32.
     - It uses the same MEC tap (ordinals 800 / 802; the kernel has only two MEC slots).
     - It hooks `XamVoice*` in XAM, where we hook `XVoiced*` in the kernel.
     - Fetch it in a scratch clone (`git fetch origin pull/7/head`) and diff against our `voice_360.c` / `game_360.c`: what would be shared, what conflicts.
     - A jack headset on a wireless pad would need GIP audio over the radio path (`XA_DONGLE_QUEUE_AUDIO` exists in `core/dongle.h:20`).
  3. One plugin for both: `xw.xex` already links `mt76.c`, `dongle.c` and `platform_360.c`. What is missing is the adapter's class-driver entry, firmware load (`xa_plugin.c:487`) and the dongle ops. Estimate size and the shared `xinput_360.c` slot model.
- Optional console step (only if the owner has the adapter and agrees): swap `xw.xex` for `xa.xex` for one boot, pair the SCUF, `xa.py status`. That is two reboots, so keep it out of S1-S4 unless the finding needs it.
- Recommendation format: go / no-go, effort (S/M/L), what to take from upstream versus keep ours.

## 4. Panel design

Row list and order:

| # | Row | Values | Default |
|---|---|---|---|
| 1 | BALANCE | GAME 5 .. EVEN .. CHAT 5 | EVEN |
| 2 | VOLUME | 0..10 | 5 |
| 3 | SIDETONE | OFF, 1..10 | 5 |
| 4 | MIC VOLUME | AUTO, 1..10 (live meter; in AUTO the value shows AUTO and the meter still moves) | AUTO after acceptance (5 until then) |
| 5 | NOISE GATE | OFF / AUTO (dot lit while open) | AUTO after acceptance (LOW until then) |
| 6 | PRESET | FLAT, then 29 names, name drawn wide | FLAT |
| 7 | SURROUND | OFF / ON (dot lit when 5.1 is arriving) | OFF until the owner has heard it |
| 8 | TEST TONE | OFF / 440 HZ / 1 KHZ | OFF |
| 9 | MIC TEST | READY / REC n SEC / PLAYING n% / "CLIP n SEC" | empty |

TEAM VOL and TEAM CH slot in after SURROUND when team voice is switched on (11 rows).

Fit, using `hp_panel_h` = 20 + (n-1) x 10 + 24 units and `geometry()` (`hud_360.c:682`: scale = (height+180)/360, safe area = middle 80 %):

| Picture | Scale | Safe height (units) | 9 rows = 124 | 11 rows = 144 |
|---|---|---|---|---|
| 640x480 | 1 | 384 | fits | fits |
| 1152x640 (Halo 3) | 2 | 256 | fits (248 px) | fits (288 px) |
| 1280x720 | 2 | 288 | fits | fits |
| 1920x1080 | 3 | 288 | fits | fits |

- Width is unchanged (240 units).
- Room is left for 2 more rows at 1152x640 before the scale would drop to 1 (164 units fits in 256, up to 13 rows).
- Help lines stay at 36 characters or fewer. `ROW_HELP` for MIC TEST must lose "10 SEC".
- The cursor still starts on BALANCE at every Share press; Share + Left / Right is unchanged.

## 5. Console sessions

Hot reload is not used (see section 9), so every session is one install and reboot, plus a spare reboot if a build is bad. `xw.py tee` runs throughout; `accept/run.py` drives everything.

| Session | Build | What the scripts do | What the owner does (all of it) |
|---|---|---|---|
| S1 "Base + probes" | `batch/S1` = base-contracts + base-split + surround probe + settings probe (+ panel-v4 and eq-presets if ready, gated off) | Regression suite; `surround_cost.py` matrix; per-channel peaks; 1.44 MB allocation under a game; XUID read on sign-in / sign-out; file write timing; (bonus) panel screenshots | Press the Xbox button. Start a Halo 3 match and play about 10 minutes. Say if game sound crackles on the TV or headset. Sign a profile out and in once when asked. |
| S2 "Batch A" | main + panel-v4 + eq-presets + settings + mictest-v2 + mic-auto | Each feature's gate switched on over xbdm, its accept script run; one scripted cold reboot in the middle for the settings test (second button press) | Press the button (twice). Look: rows readable, mute mark steady, choose its corner. Listen: one mic test with replay; talk and stay quiet; three presets in a game. Confirm settings came back; sign in a second profile. |
| S3 "Batch B" | main (S2's passes, defaults flipped) + surround + S2 fixes | Boots with S2's features on by default (verifies the flips); `accept/surround.py` (channel walk-around, on/off level match, cost) | Press the button. Listen to a sound circling the head (front-left, centre, front-right, right-back, left-back) and name where each is. Play 10 minutes with SURROUND on, then off: better, worse, same? Decide the default. |
| S4 "Release candidate" | main, all defaults final | Cold boot with no computer commands; afterwards read counters only | Press the button and play a normal match for 15 minutes with a friend in chat if possible. Report anything odd. Then tag, release, update docs. |

- Planned: 4 sessions and about 5 reboots. Realistic: 5-6 sessions.
- Anything that fails in S2 moves to S3 without blocking the rest.
- The optional wireless swap is its own short session.

## 6. Surround

### 6.1 Measurement (S1; probe on branch `surround`, all in `game_360.c` / `surround_360.c`)

Add block `XWSR`:
- Controls: `load_convs` (0..10), `load_taps` (0..128), `load_site` (0 tap, 1 driver thread), `surround`, `test_ch`.
- State: `tap_us`, `tap_max_us`, `tap_hist[6]` (<50, <100, <200, <400, <800 us, more), `tap_period_max_us`, `ch_peak[6]`, `ch_peak_max[6]`.
- The dummy load is the real inner loop (s16 x Q14 into s32) over real tap data; its result is discarded.

| # | Question | How | Pass |
|---|---|---|---|
| M1 | Does the mix buffer carry real 5.1 with the console's current audio setting? | `ch_peak[]` during Halo 3 gameplay, no setting changed | C, SL, SR above -50 dBFS at some point. If silent: ask the owner to set Dolby Digital 5.1 and repeat. If only that setting gives 5.1, it becomes a per-console setup step at the LAN; report it. |
| M2 | Baseline tap time and jitter | load 0, 60 s | record only |
| M3 | Cost in the tap | 5x32, 5x64, 10x64, 10x128, 60 s each in a match | `tap_us` p99 <= 300 us and max <= 500 us (under 10 % of the 5.33 ms frame); `tap_period_max_us` within +1 ms of M2; `capture_errors` +0; `overflow_frames` +0; `tap_rate` 187-188; HUD `swaps_per_s` not more than 1 below baseline; no crackle heard |
| M4 | Cost on the driver thread | same sizes x 384 frames per earpiece block, `load_site` 1 | `work_max_us` rise <= 1500 us; `pass_hist` over-25 ms bucket +0; `out_starved` +0; `out_frames_missed` +0; game `underruns` +0; `mic_pps` 1000 |

Decision:
- M3 passes at 5x64: design A.
- Only 5x32 passes: design A with 32 taps.
- M3 fails and M4 passes: design B.
- Neither passes: section 6.4.

### 6.2 DSP design (recommended: A)

- The work is 5 convolutions, not 10. With a left/right-symmetric head, a speaker pair is processed as sum and difference: S = (L+R)/2 and D = (L-R)/2, each through one filter; then outL = S' + D', outR = S' - D'.
  - Front pair: 2 filters. Surround pair: 2 filters. Centre: 1 filter (same in both ears).
  - HANDOFF 4.5 halved only the table; this halves the multiplies too.
- Cost: 5 x 64 taps x 48000 = 15.4 M multiply-adds a second (HANDOFF's figure was 61 M). My estimate, unmeasured: 0.1-0.3 ms per 256-frame block.
- Fixed point: s16 samples x s16 Q14 coefficients, s32 accumulator. With sum|h| <= 2.0 per filter the accumulator stays under 2^30, so no 64-bit maths is needed. The generator enforces this. Output trim `out_q14`, then clamp.
- Where (design A): in the tap, replacing `convert_frame` when SURROUND is on. Convert the 6 channels to s16, run the 5 FIRs over a contiguous history + block buffer (no modulo in the inner loop; state malloc'd at init), write stereo into the existing ring.
  - Everything after the ring (`xg_mix`, drift trim, buffer target) is untouched.
  - OFF is today's code path, byte for byte.
  - The render thread is never preempted, so timing is deterministic.
- Design B (fallback): the tap writes 5 channels of s16 into a new malloc'd ring (8192 frames x 5 x 2 = 82 KB); `xg_mix` virtualizes before its resampler. More code, a bigger ring, and the driver thread's 15.6 ms pauses to respect.
- With SURROUND on, always virtualize; a stereo game simply gets two virtual front speakers. No automatic bypass, so the sound never changes character mid-game. `surround_live` only lights the dot.
- Switching on/off: compute both for one 256-frame block and crossfade.
- Loudness: the generator matches on and off for front-channel pink noise.
- LFE: today's downmix drops it (`game_360.c:119`). Add it to both ears at `lfe_q14` in surround; default -6 dB. Owner to judge in S3.
- No room tail in version 1.
- Acceptance aid: `surround.py walk` injects a noise burst into one channel at a time ahead of the virtualizer. This is how the owner hears it working.

### 6.3 HRTF data

| Source | Licence (verify on the download page before bundling) | Notes |
|---|---|---|
| SADIE II (University of York), KU100 dummy head | Apache 2.0, I believe | 48 kHz versions exist; my first choice |
| MIT KEMAR (Gardner and Martin, 1994) | Free use with citation | 44.1 kHz, needs resampling; the "compact" set is 128 taps; safe fallback |
| CIPIC | UC Regents notice, free use with notice, as I recall | 44.1 kHz |
| TH Koeln KU100 (Bernschuetz) | CC BY-SA 3.0, as I recall | Share-alike on data; avoid unless needed |
| IRCAM Listen, HeSuVi room sets | Unclear or research-only | Do not bundle |

Generator (`tools/hrtf/gen.py`, numpy on the Mac):
- Pick 0, +/-30 and +/-110 degrees at ear level.
- Resample to 48 kHz if needed; remove the common leading delay; window to 64 taps (the interaural delay is at most about 33 samples, so 64 holds delay plus response).
- Diffuse-field equalise; form the sum / difference filters; normalise; quantise to Q14; write `surround_tab.h` and a credit line.
- Off-console checks: the integer C compiled on the host is bit-exact with the Python model. A left-only input gives the expected interaural level and delay. Front-left versus surround-left differ measurably in spectrum. No clipping on full-scale 5-channel noise.
- Table size: 5 x 64 x 2 = 640 bytes.

### 6.4 If the CPU budget is too tight

1. 32 taps (minimum-phase responses plus an explicit sample delay for the far ear).
2. 24 taps on the surround pair only.
3. Hand-written VMX in a `.s` file for the inner loop. Unverified on two counts: clang's soft-float build cannot emit VMX, so it would be hand assembly; and I have not confirmed the kernel keeps VMX state for these threads. Check in the 17559 dump before spending time.
4. Cut to "wide stereo" (crossfeed with 2 short filters) or drop the row.

## 7. Top risks, and what to cut first

| Risk | Effect | Mitigation |
|---|---|---|
| 5.1 only arrives with a console audio setting changed | A per-console setup step at the LAN | M1 first; if true, tell the owner before building further |
| Surround sounds "phasey" without a room | Owner disappointed | Walk-around demo; default OFF; diffuse-field equalisation |
| Work on the render thread disturbs TV audio | Crackle for everyone | Pass numbers in M3; design B |
| Persistent mute mark flickers or costs frame rate | Annoying | `accept/badge.py`; periodic-show fallback |
| XAM XUID call from the wrong thread or process type | Freeze | Probe in S1, behind a gate |
| File write stalls | Audio gap | Worker thread; latency counter |
| Preset band semantics guessed wrong | Presets sound wrong | Prove in W0; say so if unproven |
| Contract error found after W1 starts | Rework for five workers | Orchestrator reviews section 2 before merging BASE-1; reserved bytes in every struct |
| Image size | Build stops loading | Per-package size report; canvases malloc'd; the 128 KB `rx_ring` as reserve |
| Split of `voice_360.c` changes timing | Regressions | S1 regression suite before anything else |
| AGC pumping | Teammates hear swelling noise | Offline tuning on real recordings; ship gate AUTO + FIR alone if needed |

Cut order, first to last:
1. Comb canceller.
2. Burn-in shift.
3. AGC seeding from the mic test.
4. Preset author field display.
5. LFE in surround.
6. MIC VOLUME AUTO (keep gate AUTO + FIR).
7. The 30 s clip (keep 10 s with replay).
8. Surround.

## 8. Repo contradictions and stale items found

1. Image size: `CLAUDE.md:32` and `HANDOFF.md:27` say 0xdd000. The built `auto-quiet` image is 0xde000.
2. HANDOFF 4.2 says "`xa_plugin.c` already does NtCreateFile-style file I/O: reuse". It is not linked into xw.xex and only reads. The only write code is in the test titles (`echo.c:199`, `voicetest.c:162`).
3. HANDOFF 4.2 says WP3.4 "can share the same struct". There is no such struct: `handover.h` has no settings; the carry-over is done from the Mac by `xw.py` `knob_specs` (`xw.py:834`). New settings need adding there, or reload should rely on the saved file.
4. HANDOFF 4.5 says to measure with XWDG `work_us` / `pass_us`. Those time the driver thread; a load in the tap needs its own timing.
5. `CLAUDE.md` says "Git: work on `main`" and "One branch: `main`". The owner's rule is feature branches, and two unmerged branches exist.
6. `CLAUDE.md` "Hardware verified" and HANDOFF section 5 say the SCUF was only run with the Mac tool. `pad_quirks.h:573-576` says its Share byte was seen on the console on Nov 23 2026, and the defaults were "noted on the SCUF".
7. `INTERNALS.md` says notifications never run on the driver thread. Since `notify-fix`, `xinput_360.c:392-411` runs them inline on the driver thread on purpose.
8. `PLAN.md` 1.0 says LB / RB were dropped. `panel_360.c` has `bumpers = 1` by default (sidetone on LB / RB).
9. Stale comments:
   - `voice_360.c:21` says stage 0 is the default; it is 4.
   - `hud_360.c` says stage 0 at boot; it is 3.
   - `voice.py` says the mic test is "off at boot until accept/mictest.py has passed"; `enable` is 1.
   - `game_360.h` says "Settings live in memory only".
   - `hud_panel.c` help text says "RECORDS 10 SEC".
   - The `hud_360.h` assert message says "hud.py test state".
10. HANDOFF section 5 mentions a long status section in `CLAUDE.md`; it is now a 63-line guide with none.
11. There are `HUD_HOST_TEST`, `XG_HOST_TEST` and `XV_HOST_TEST` switches in the sources, but `tests/` has harnesses only for team voice and SHA-256. Host rendering and audio tests must be written (BASE-2 and the feature packages).
12. Not listed anywhere: chat from the 360 goes 8 kHz to 48 kHz by plain linear interpolation (`rs_ear`, `voice_360.c:468`), the mirror image of the mic aliasing bug. Cheap to fix with the same FIR once MIC-AUTO has it; added to the to-do list.
13. HANDOFF 4.1 leaves "Left while recording" ambiguous (stop, or discard). This plan uses discard; confirm with the owner.

## 9. To-do list (not planned here)

| Item | Depends on |
|---|---|
| Team voice verified on two consoles, then a "who's talking" indicator | A second console; `hud_state.team_talking` already exists; the badge canvas can carry it (`badge` bits are free) |
| More than one headset per console | The 12-iTD pool limit (PLAN "Later"); settings and the panel are single-headset today, and the settings key is already per XUID |
| Volume limiter / night mode | The reserved slot after `xq_process` (section 2.4) |
| Controller battery / connection status on the panel | A new row; room for 2 more rows (section 4) |
| Update from a USB stick | `file_360.c` (SETTINGS) and `xw.py install`'s file list |
| OBS capture and admin talk-back | Team voice verified (`XT_F_ALLCALL`, `XT_F_PC` in PLAN 4.1) |
| Shared database of player-submitted presets | The `xw.presets` format and `tools/presets` converter |
| Noise suppression on the mic | MIC-AUTO's floor tracker and the 24 kHz mono stage before `shape()` |
| Chat 8 to 48 kHz interpolation filter | MIC-AUTO's FIR code |
| A way to keep the panel off without a computer (`Hdd:\Plugins\xw.safe`, HANDOFF 4.0) | `file_360.c` |
| Offer upstream the wired support and small fixes | The WIRELESS finding (it decides what is shared); never push to `origin` |
| Hot reload fix (PLAN Phase 3) | Would not pay for itself in this block. This plan needs about 5 reboots, and the owner has to be present to listen in every session anyway. PLAN's own E1-E7 experiments each risk a freeze plus reboot, and the first reload with headset audio froze the console. Do it after S4 as its own block. Until then every new module ships quiesce / resume so it does not get harder. |

### Critical Files for Implementation
- /Users/owner/Documents/code/AmpJack/xbox360/voice_360.c
- /Users/owner/Documents/code/AmpJack/xbox360/hud_360.h
- /Users/owner/Documents/code/AmpJack/xbox360/game_360.c
- /Users/owner/Documents/code/AmpJack/xbox360/panel_360.c
- /Users/owner/Documents/code/AmpJack/xbox360/hud_panel.c

---

# Part 2: addendum (owner's scope changes; wins over Part 1)

This amends the plan already delivered; everything not mentioned here stands. Still read-only: nothing was changed or run on the console. Line counts are measured; image-byte savings marked "est." are my estimates, not measured.

## A. AUTO is added, nothing existing changes

- **NOISE GATE** = AUTO / OFF / LOW / HIGH. **MIC VOLUME** = AUTO / 1..10. The manual values run today's code path untouched.
- Contract changes to section 2 of the plan:
  - `g_xd.gate` keeps 0 OFF, 1 LOW, 2 HIGH and adds 3 AUTO. `HUD_GATE_*` is the same.
  - The panel shows the gate in the order AUTO, OFF, LOW, HIGH through a small order table in `panel_360.c`; the stored numbers do not move, so xbdm tools and old scripts keep working.
  - MIC VOLUME: `hud_state.mic_level` 0 = AUTO (left of 1), 1..10 as today. `g_xd.mic_gain` keeps the manual value while AUTO is on, so leaving AUTO returns to the player's last manual step.
  - PANEL row logic: Left / Right steps through all four gate values (the earlier "OFF / AUTO only" is withdrawn).
- MIC-AUTO acceptance gains a regression check: with gate LOW or HIGH and MIC VOLUME 1..10, the thresholds and gains are the same numbers as today (`gate_threshold` 184 / 583, `mic_gain_q15` per step), and `accept/wp1_4.py` passes unchanged.
- **Decision for the owner:** compiled defaults stay gate LOW and MIC VOLUME 5 (his noted settings) until he has heard AUTO in session S2. The coordinator proposes AUTO as the default for both. Ask him then; the flip is a one-line commit.

## B. Wireless: investigation removed, breadcrumbs only (S)

- The WIRELESS package and its optional console session are deleted. No hardware work.
- New task BREADCRUMBS rides inside BASE-1 (it already owns the headers and `build.sh`, so no other worker's file is touched):
  - `docs/WIRELESS-NOTES.md`, about one page, linked from `CLAUDE.md` "Read first".
  - Pointer comments of two or three lines each, in headers only: `xinput_360.h`, `game_360.h`, `voice_360.h`, `pad_quirks.h`, and next to `XW_SRC` in `build.sh`.
- What the note should record (all from reading; none tested):
  - `xw.xex` and `xa.xex` cannot load together because both install the three XInput hooks from `xinput_360.c`. One plugin would need both class-driver entries in one image sharing that file's 4 slots.
  - `xw.xex` already links `core/mt76.c`, `core/dongle.c` and `platform_360.c` (`build.sh`: `$XW_SRC $CORE`). Missing for wireless: the adapter's class-driver match, the firmware loader (`xa_plugin.c:487`) and the dongle ops (`xa_plugin.c` `dongle_ops`).
  - Headset audio over the adapter: upstream PR #7 on Jacckii/Xbox360-Series-Controllers-Native is prior art. It is open, by nobodys-coder, for the Xbox Wireless Headset 045e:0b32; game audio works, mic untested.
    - It uses the same kernel audio tap as `game_360.c` (ordinals 800 / 802); the kernel has only two such slots.
    - It hooks `XamVoice*` in XAM, where `voice_360.c` hooks `XVoiced*` in the kernel. The two would need reconciling.
    - It puts its driver thread on hardware thread 3 because "thread 1 is the title's main thread". Our HUD draw thread is on thread 1.
  - A jack headset on a wireless pad would need GIP audio on the radio queue (`XA_DONGLE_QUEUE_AUDIO`, `core/dongle.h:20`). `core/gip.c`'s headset code is transport-neutral (`enable_audio` / `send_audio` ops).
  - SCUF Instinct Pro wireless: untested. Share is assumed to be at the same report byte (`pad_quirks.h`).
- The freed Wave 0 slot goes to QUALITY-TOOLS (section C.5).

## C. Code-quality track

### C.1 Audit: where the bulk is

Tracked source, by area:

| Area | Lines | Notes |
|---|---|---|
| `xbox360/*.c` + `*.h` (plugin, loaders, test titles) | about 19,000 | `xw_plugin.c` 3588, `voice_360.c` 3019, `hud_360.c` 2007, `team_360.c` 1716 |
| `core/` | about 5,900 | upstream's; leave alone ("upstream wins") |
| Python tools + `accept/` | 8,636 | `xw.py` 1839, `voice.py` 654, `team.py` 643, `hud.py` 410 |
| `docs/*.md` + `research/*.md` | 3,720 | `HOT-RELOAD.md` 743, `PLAN.md` 676, `AUDIO.md` 696, `DISCORD.md` 500 |

- Image: 0xde000 = 909 KB, of which code is 246 KB, constants 23 KB, and data 613 KB. The data is mostly zero-filled buffers that count as image because of the no-.bss rule.
- Comment lines are about 10 % of the plugin sources (for example 349 of 3588 in `xw_plugin.c`). Density is not the problem; many comments narrate history ("fifth console run", "Nov 22 2026", "was reserved[0]").

Removable or dev-only candidates:

| # | What | Where | Size | Recommendation |
|---|---|---|---|---|
| 1 | Hot reload: both sides, handover block, 71 `trace()` calls, slot builds | `xw_plugin.c` 2650-3350 (about 700 lines), `handover.h` 173, `xw.py` 561-1316 (about 750), `build.sh` slot section, `accept/p3_*.py` 225, `docs/HOT-RELOAD.md` 743 | about 2,600 lines; est. 15-25 KB of code | Does not work with a headset. Fence behind `XW_DEV` now; delete or fix in the hot-reload block later. Keep the detour slot mechanism itself: it is how hooks are installed. |
| 2 | Mic dump buffer | `xw_plugin.c:318-327`, `g_micdump` | 64 KB of image, exact (`MICDUMP_MAX` 0x10000) | Dev only: malloc on demand under `XW_DEV`. |
| 3 | Audio self-test (tone out, analysis in) | `xw_plugin.c` 1867-2022 | about 155 lines | Dev only. |
| 4 | `usbreset`, `hsrestart`, `XW_STAGED` gates, `XW_NO_AUTH` | `xw_plugin.c` 2023-2095 and scattered | about 150 lines | Dev only. |
| 5 | HUD bring-up modes: `mode`, `src`, `alias`, `order`, `wide`, `noflush`, `thread` 0, `bg`, `keep`, `retry_us`, and the 64-event trace | `hud_360.c`; XH36 is 457 words, 256 of them the trace | est. 400-600 of 2007 lines | Only one configuration was verified on the console. Fix it in release; keep the alternatives under `XW_DEV`. Highest-risk cleanup: every knob exists because a console run failed without it. |
| 6 | Voice stages 1-3, stock-headset watching ("R2"), loopback, u8 / mu-law / big-endian formats, the debug event ring | `voice_360.c` 559-770, 1069-1104 and parts of the status block | est. 300-400 lines | Release always runs stage 4. Dev only, after checking which formats a title can really ask for (`research/AUDIO.md`). |
| 7 | Loop-timing probe (`probe`, sleep-granularity test) | `voice_360.c` 2593-2755, `diag_360.h` comment 237-263 | about 160 lines | Keep `pass_us` / `work_us` (they are the overhead scorecard); the probe is dev only. |
| 8 | Software sidetone and its feedback guard | `voice_360.c` 1369-1750 | about 380 lines | **Keep.** It is the fallback for headsets whose own sidetone is not writable. |
| 9 | Test tone row | `voice_360.c` 2019-2083, panel row | about 100 lines | Keep (it is a player-facing row). Offer the owner the option to drop it and free a panel line. |
| 10 | Team voice, unverified | `team_360.c` 1716, `net_360.c` 318, `team.py` 643, `tests/` 593, `accept/p4_*` 884, `docs/TEAM.md` 149 | about 4,300 lines | `build.sh` already switches to `team_360_stub.c` when the file is absent. Publish with the stub (est. 15-20 KB less code) until it is verified on two consoles. Keep the source. |
| 11 | Wireless plugin and bring-up titles in every build | `xa_plugin.c` 1106, `m3_probe.cpp`, `echo.c` 926, `voicetest.c` 747, `loader.c` | separate binaries; no effect on `xw.xex` | Upstream's or test tools: keep. Build the test titles only with `XW_DEV`. |
| 12 | Old acceptance scripts for finished bring-up | `accept/wp0_1.py`, `wp0_2.py`, `wp1_1.py` | 826 lines | Retire to git history. Keep `wp1_3`, `wp1_4`, `mictest`, `sidetone`, `p1_panel` as the regression suite. |
| 13 | Python tools: `xw.py reload` / `trace` / `gate` / `hooks`, `headset_test.py` 183, `echo.py` 229; `shot.py` decodes Halo 3 wrongly and `--find` is broken (HANDOFF section 2) | | about 1,400 lines | Move reload with item 1; fix or drop `shot.py`. |
| 14 | Docs that are history: `PLAN.md` phases 0-2 done; `HOT-RELOAD.md`; `research/DISCORD.md` 500; `research/` is 9.2 MB on disk | | | Move finished plans to `docs/history/`; keep `INTERNALS.md`, `WIRED.md` and a short `HANDOFF.md` current. One page per unfinished feature. |
| 15 | Duplicated helpers: `clamp16` (`voice_360.c:461`, `game_360.c:265`); NV64 thunk macros re-declared in `voice_360.c:247`, `game_360.c:219` and `thunks_common.s`; `BARRIER` / `HOOK` in four files; the `struct sig` code-check pattern in three; `isqrt` / `isin` private to `voice_360.c` | | est. 150 lines | One small `dsp_360.h` (integer helpers) and `kernel_360.h` (barrier, hook, signature check) in BASE-2. The new EQ, surround and mic code need them anyway. |
| 16 | Big zero-filled statics that inflate the image | `rx_ring` 128 KB (`platform_360.c:208`), HUD canvases 59 KB, `g_ring` 32 KB | 219 KB of image | malloc at init. RAM use is unchanged; the image shrinks. |
| 17 | 93 `xa_log` calls in `xw_plugin.c`, 38 in `voice_360.c`; the first-run "defaults" banners | | format strings are part of the 23 KB of constants | Keep error and state-change logs; put chatter under `XW_DEV`. |
| 18 | Stale or history-narrating comments (plan section 8, items 9 and 10) | | | Rewrite to say why, in the file that owns the code. |

- Not candidates: thunks, `detour.c`, the Usbd request ring, transfer-buffer sizing, the NULL control buffer, `xa_kcall9`, the 20 s and 8 s boot holds, every signature check, the packet write order and `lwsync` barriers, the notify TLS binding, `AUD_OUT_BLOCKS` 5, the draw-thread settings. All of these are in `INTERNALS.md` or the research notes as hard-won.
- Rough total reachable without touching behaviour: 219 KB of image from item 16, 64 KB from item 2, and an estimated 40-60 KB of code from items 1, 3-7 and 10. That would put a release image near 0x90000 with the new features included. To be measured by the size tool, not promised.

### C.2 When: mostly agree, with one change

Agree: the split and helper consolidation go first, inside BASE-2. They remove the merge problem and shrink what each worker must read.

Disagree on doing the deletions first. Three reasons:
- The S1 measurements and every acceptance script depend on the dev machinery (inject, status blocks, probes, tee).
- Deleting inside files while five workers branch from them recreates the collision problem.
- If a regression shows up in S2, "was it the cleanup or the feature" costs a console session to answer.

So three steps:

| Step | When | What | How it is proven behaviour-preserving |
|---|---|---|---|
| Q1 Split + fence | BASE-2, before parallel work | Pure moves (`voice_360.c` into four files; `xw_plugin.c` reload and self-test regions into `reload_360.c` and `selftest_360.c`); shared helper headers; items 2, 16; wrap items 1, 3, 4, 7 in `#ifdef XW_DEV` without editing their bodies. Sessions S1-S3 run `XW_DEV=1` builds. | Per-function disassembly diff before and after (C.5): every function body identical apart from addresses. The `XW_DEV=1` build must show zero changed functions against the pre-split build, except where an allocation replaced a static. Then S1's regression suite, plus a counter baseline (`work_us`, `pass_hist`, `out_starved`, `mic_pps`, HUD `pres_pct`) compared with the same counters taken on today's build at the start of S1. |
| Q2 Lean rules | During W1 / W2 | C.3 applies to all new code | The review step in C.3 |
| Q3 Release pass | After S3, before S4 | First build with `XW_DEV` off. Then the riskier trims, one commit each: item 5 (HUD modes), item 6 (voice stages and formats), item 10 (team stub), item 17, items 12-14 and 18 (scripts, tools, docs, comments). | The disassembly diff shows only removed functions plus the named edits. S4 becomes two boots: the dev build of the same commit runs the full acceptance suite; then the release build boots with no computer and the owner plays. Release keeps the read-only status blocks and XWPN `inject`, so the scripts that only read and press buttons still run against it. Any trim that fails is reverted on its own. |

Items 5 and 6 are the ones I would let slip to after publishing if S4 is tight. They save lines, not much overhead, and they are where "odd but load-bearing" is most likely.

### C.3 Rules for workers (add to the per-package rules)

1. **Tiers.**
   - Release contains: the feature; one status block per module with controls, the state a script needs, and error counters; error and state-change logs.
   - `XW_DEV` contains: probes, dummy loads, traces, histograms, dump buffers, alternative modes, verbose logs.
   - If a field or knob has no reader in a tool or acceptance script, it does not exist.
2. **Diagnostics budget:** at most 24 words of status per new module in release; anything more is `XW_DEV`.
3. **Comments:** say why, and what breaks if it is changed. No dates, run numbers, branch names or "was X before". History belongs in the commit message. One header comment per file stating its thread and locking rules.
4. **Budgets per package**, reported in the hand-back (measured with the size tool and XWDG):

   | Package | Image | Heap | Driver-thread work per pass | Other |
   |---|---|---|---|---|
   | PANEL | 6 KB | 0 (canvases move to heap in BASE-2) | +50 us | mute mark draw under 0.25 ms |
   | MICTEST | 3 KB | 1.44 MB only while a clip exists | +20 us | |
   | MIC-AUTO | 5 KB | 4 KB | +100 us | chat mic delay +1 ms at most (FIR) |
   | EQ | 4 KB | 8 KB | +300 us | 0 added delay |
   | SETTINGS | 6 KB | 4 KB | +10 us | file work off the driver thread |
   | SURROUND | 6 KB | 16 KB | 0 (design A) | tap +300 us p99; delay under 1.4 ms (64 taps) |

   Going over means asking the orchestrator, not silently shipping.
5. **No duplicate helpers:** use `dsp_360.h` / `kernel_360.h`; add to them instead of copying.
6. **Files:** no new file over about 600 lines; pure DSP in functions with no globals, so they are host-testable.
7. **Review before merge:** one separate Opus reviewer per package, or a `/simplify` pass by a fresh agent. Checklist: these rules, the kernel rules in `INTERNALS.md`, the budgets, the append-only status rule, and whether anything can be deleted. The reviewer may not widen scope.
8. **MICTEST change from the plan:** free the 1.44 MB buffer when the clip is discarded or the headset leaves, instead of keeping it for good.

### C.4 "Low overhead", as numbers the owner can be given

Report one table at S1 (baseline) and again at S4 (release), with one row per feature showing its delta when switched on:

| Measure | How it is read | Baseline today | Target for release |
|---|---|---|---|
| Plugin size on disk / in memory | build output, `check_xex.py` | 0xde000 (909 KB) | below today's with every new feature in; hard limit 0x100000 |
| Extra memory while running | sum of the modules' allocation counters (add one `heap_bytes` word per block) | not known: measure in S1 | under 400 KB idle; +1.44 MB only while a mic-test clip is held |
| Driver-thread work per pass | XWDG `work_us`, `work_max_us`, `pass_hist` | measure in S1 | average rise under 0.5 ms with everything on; no new passes over 25 ms |
| Game-audio thread time | new `tap_us` p99 / max (XWSR) | measure in S1 | under 300 us p99 of a 5.33 ms frame with surround on |
| Picture cost | XH36 draw time, `swaps_per_s` with panel, mute mark, and nothing shown | Halo 3 30 a second | unchanged frame rate; panel draw under 1 ms; mark under 0.25 ms |
| Added sound delay | computed and logged: game buffer 45 ms (unchanged), EQ 0, surround under 1.4 ms, mic filter under 1 ms | 45 ms game path | no more than +2.5 ms in total |
| Dropouts | `out_starved`, `out_frames_missed`, game `underruns`, `mic_pps` over a 15-minute match | 0 / 0 / 0 / 1000 | the same |

Plain-language line for the owner: "how big it is, how much memory it takes, how much of the console's time it uses, how much later sound arrives, and whether anything ever drops out".

### C.5 QUALITY-TOOLS (S, Wave 0, replaces the wireless slot; Python only, new files)

- `tools/size_report.py`: per-source-file and per-symbol bytes from the build (link map or `llvm-nm --size-sort` on `out/xw.exe`), text / constants / data split, diff against a saved baseline.
- `tools/fn_diff.py`: disassemble two builds, normalise addresses and relocations, list functions that are identical, changed, added or removed. This is the proof for Q1 and Q3.
- `accept/overhead.py`: reads the C.4 counters into one table and compares with a stored baseline.
- Owns only those new files. Check `toolchain/build.sh` for whether the link map is easy to emit; if not, `llvm-nm` on the PE is enough.

## D. Net changes to waves and sessions

| Wave | Worker 1 | Worker 2 | Worker 3 | Worker 4 | Worker 5 |
|---|---|---|---|---|---|
| W0 | BASE-1 + BREADCRUMBS | QUALITY-TOOLS | EQ offline | HRTF offline | MIC-AUTO offline |
| W1 | BASE-2 (now includes Q1: split, helpers, fencing, heap moves) | PANEL | EQ plugin | SURROUND probe, then engine | SETTINGS probe, then feature |
| W2 | MICTEST | MIC-AUTO plugin | SURROUND engine | SETTINGS finish | package reviewers / fixes |
| W3 "Release" (new, after S3) | Q3 trims, one commit each | docs, tools and comments pass | final reviewer | | |

- BASE-2 grows from M to L. It is the riskiest package: the orchestrator should give it the strongest worker and review the `fn_diff` output personally.
- S1 gains the counter baseline on today's build. That means installing nothing first: read counters for five minutes on the running build, then install the batch.
- S4 becomes two boots (dev build with the full suite, then release build with no computer).
- Planned total: 4 sessions, about 6 reboots.
- Cut list addition: Q3 items 5 and 6 are cut before any feature is.

### Critical Files for Implementation
- /Users/owner/Documents/code/AmpJack/xbox360/voice_360.c
- /Users/owner/Documents/code/AmpJack/xbox360/xw_plugin.c
- /Users/owner/Documents/code/AmpJack/xbox360/hud_360.c
- /Users/owner/Documents/code/AmpJack/xbox360/build.sh
- /Users/owner/Documents/code/AmpJack/xbox360/diag_360.h

---

# Part 3: owner's decisions (Nov 23 2026, night; win over Parts 1 and 2)

1. **Defaults stay as they are** (NOISE GATE LOW, MIC VOLUME 5, VOLUME 5, SIDETONE 5) until
   AUTO has been proven useful to him. AUTO ships as a value a player can pick. No default
   flips without his say.
2. **MIC TEST is record / play / pause, and the clip never outlives the menu:**
   - Right: nothing held -> start recording; recording -> stop and play it; playing -> pause;
     paused -> resume; finished (clip held) -> play again.
   - Left: recording -> stop and discard; playing / paused / held -> discard.
   - Auto-stop at 30 s (10 s if the big allocation fails), then it plays once.
   - Letting go of Share while it records or plays keeps it going (the toast shows it), as
     today. **The clip is deleted, and its memory freed, when the panel closes with nothing
     recording or playing** (so: paused or held + Share released = gone). A clip that finishes
     while the panel is hidden is kept until the panel has next been opened and closed.
   - States for the contracts: IDLE, RECORDING, PLAYING, PAUSED, READY (clip held). This
     replaces the READY-only model in Part 1 2.1 / 2.3: add `HUD_MT_PAUSED` /
     `XD_MT_PAUSED` and `xd_mictest_pause()` / resume (or one toggle), and a
     `xd_mictest_panel_closed()` hook called by panel_360.c.
3. **Diagnostics stay in the code for future agents.** The code-quality track fences
   development-only machinery behind `XW_DEV` and documents how to build with it
   (`docs/DEVELOPMENT.md`); it does not delete diagnostics. Deleting is only for dead code
   (abandoned experiments nobody can use), stale comments, and scripts for removed features.
4. **Team voice is not published until it has been worked on:** release builds link
   `team_360_stub.c`; the source, tests and tools stay in the repo.
5. **Hiss fix (`dev_quiet`) and the notification fix are accepted** and merged to `main`
   (release `wired-v3`).
6. **Autonomous overnight run.** the owner is asleep: nobody presses the controller's button or
   listens. After a reboot the pad stays dark, so the console has no headset. Console use is
   therefore limited to boot / regression checks that need no pad; everything that needs his
   ears or hands is queued for a session with him (`docs/SESSION-NOTES.md`, kept by the
   orchestrator). If the console freezes or does not come back, **do not wait for it**: note
   it and keep working off-console.
7. **Integration branch `next`.** `main` only ever holds work verified on the console with
   the owner. During the run, reviewed packages merge into `next` (each new feature gated off by
   default, or behaviour-preserving). Workers branch from `next`. `next` goes to `main`
   feature by feature after his sessions.
