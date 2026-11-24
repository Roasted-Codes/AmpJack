# MIC-AUTO: design, reference model, evaluation, integration

Offline part of the MIC-AUTO package (ROADMAP Part 1 "MIC-AUTO", Part 2 A, Part 3):
NOISE GATE AUTO, MIC VOLUME AUTO, a real 24 -> 8 kHz low-pass for chat, and an optional comb
canceller. The plugin side is `xbox360/micdsp_360.c` (section 4).

| File | What |
|---|---|
| `xbox360/micauto_dsp.h` | The kernels: integer C, no globals, no FP, no kernel includes. `micdsp_360.c` includes it. |
| `tools/micdsp/micdsp.py` | Integer Python model of the header (reads its constants and tables from it) and of today's mic path. Plain Python. |
| `tools/micdsp/vectors.py` -> `vectors.h` | Test vectors from the model (generated: do not edit). |
| `tests/test_micdsp.c` | Host test: C bit-exact with the model; the manual settings are untouched; overflow; `--bench`. |
| `tests/test_micdsp360.c` | Host test of the plugin's `micdsp_360.c` as built: the model's scenarios, the switches, and the manual path against the pre-MIC-AUTO code. |
| `tools/micdsp/design.py` | Designs the FIR and the log / exp tables, checks the header against them (numpy, scipy). |
| `tools/micdsp/eval.py`, `signals.py` | The evaluation below, and `tune`: the grid behind the gate's constants (numpy, scipy). |

```
python3 tools/micdsp/vectors.py                       # after changing the header
cc -O2 -Wall -o test_micdsp tests/test_micdsp.c && ./test_micdsp [--bench]
<venv>/bin/python tools/micdsp/design.py [--print] [--interp]
<venv>/bin/python tools/micdsp/eval.py [gate|agc|fir|comb|real|tune|all]   # all: ~5 min
```

## 1. Chosen constants and why

All levels are 20 ms window RMS in dBFS, measured after the DC removal and before any gain
(the manual gate's measurement point).

| Constant | Value | Why |
|---|---|---|
| Floor smoothing | 2^-4 per window (~320 ms) | The minimum of raw 20 ms levels of a fluctuating noise sits 6..9 dB under its mean. Unsmoothed (2^-1), floor + 8 dB opened on pink noise 31 % of the time and on room babble 86..89 %. |
| Floor history | 6 blocks x 0.5 s = 3 s | As specified; a louder room is followed after 4.0 s, a quieter one after 0.3 s. |
| Margin | + 9 dB | `eval.py tune`: with 2^-4 smoothing, 8 dB still opened on pink noise 7..14 % and babble 6..8 % of the time; 10 dB slowed non-stop talk (p95 61 ms) for 1 % less babble. 9 dB: babble alone 2..3 %, pink -60 0 %. |
| Clamp | -60 .. -35 dBFS | -60: the headset's idle noise peaks at -63 (raw_idle.wav, window max), quiet syllables reach -55. -35 (the old HIGH): above it normal speech is cut. Re-checked in section 7: neither end moves. |
| Start | threshold -45 (= step 4) until the floor is known | A quieter floor pulls it down at the first window, so it only matters in a loud room, which is then gated from the start. Started at -55 (step 2) instead, AUTO stood open on a -50 dBFS room for 3.5 s. |
| AGC speech windows | >= floor + 12 dB and >= AUTO's threshold, whatever the gate is set to | 10 dB let pause noise (-64..-66 in the owner's recording) in. |
| AGC level | mean of window levels in dB over 256 speech windows (5 s of speech), each clipped to +6 / -10 dB of the level so far | A power mean let a 2.5 s shout (+12 dB) move the level ~10 dB: the gain dipped 4 dB and took ~30 s to recover. Clipped dB mean: no dip. |
| AGC target | -31.5 dB on that mean | Gives -30.1 dBFS active speech level (P.56 style) on the owner's real recording. |
| AGC speed | 4.9 dB/s for the first 3 s of speech after AUTO starts, then 0.98 dB/s; dead band 1 dB with hysteresis | Settles in 1..7 s; no hunting. |
| AGC range | Q12 1024 .. 22937 (25 .. 560 %) | MIC VOLUME 1 .. 10. |
| FIR | 45 taps, Q15, equiripple, stop weight 30 | +0.00..+0.35 dB to 3.2 kHz, >= 62 dB down above 4.6 kHz, 0.917 ms delay. |
| Comb | 192-sample period, 2^-6 (0.5 s) | As in `research/tools/cancel.py`. |

## 2. Evaluation

`eval.py` output, condensed, as measured when MIC-AUTO was designed: "LOW" was -45 dBFS with
a 200 ms hold and the gate opened at a window's end; section 7 has the gate as it is now. A
word is "opened" if the gate opened at any time during it; delay is from the word's first
sample to the gate fully open (window end + 10 ms ramp).

### What the recordings can and cannot show

- `owner_mictest_g50.wav` (10 s, owner talking, headset mic gain 50, no digital silence in it: apparently gate OFF): the only real
  speech with its real noise floor (-76 dBFS) between words. Good for the gate and the AGC
  level; one talker, one room, 10 s, so it says nothing about LAN noise or settling over minutes.
- `owner_mictest.wav`: already through gate LOW (pauses are digital zero, word starts already
  chopped): usable only for the AGC's level. With the real idle noise added it gives the same.
- `raw_idle*.wav`: 1 s of raw idle mic, shorter than the 3 s floor window, so they were looped.
  They show the idle floor and that nothing opens on it; not how the floor tracks.
- `mic-tone-*.wav`: a steady -32 dBFS tone. Shows the known limit of minimum statistics: a
  sound that never pauses becomes "the floor" after 3 s (the threshold clamps at -35).
- No recording has a noisy room, breaths, or a level change: those are synthetic (`signals.py`:
  voiced syllables with formants and soft fricative starts, 8-talker babble, breaths, floor steps).
  Synthetic speech has less level spread than real speech, so its AGC output lands 2..3 dB
  hotter than the owner's; the target is calibrated on the real recording.

### NOISE GATE (40 s of speech per row)

| Scene | Setting | Words opened | Delay median / p95 | Pauses shut by 0.3 s | Open on noise alone |
|---|---|---|---|---|---|
| speech -40 over pink -73 | LOW | 31 / 31 | 48 / 139 ms | 12 / 12 | 0 % |
| | AUTO (thr -60) | 31 / 31 | 33 / 45 ms | 12 / 12 | 0 % |
| speech -40 over pink -60 | LOW | 31 / 31 | 48 / 139 ms | 12 / 12 | 0 % |
| | AUTO (thr -54) | 31 / 31 | 35 / 74 ms | 12 / 12 | 0 % |
| quiet talker: -50 over -73 | LOW | 15 / 31 | 344 / 667 ms | 12 / 12 | 0 % |
| | AUTO (thr -60) | 31 / 31 | 37 / 125 ms | 12 / 12 | 0 % |
| speech -40 over pink -50 (10 dB SNR) | LOW | 31 / 31 | 45 / 133 ms | 12 / 12 | 0 % |
| | AUTO (thr -44) | 31 / 31 | 53 / 133 ms | 12 / 12 | 0 % |
| LAN babble -55, player -38 | LOW / AUTO | 27 / 27 both | 43 / 131, 39 / 115 ms | 11 / 11 both | 0 % / 0 % |
| LAN babble -50, player -38 | LOW / AUTO | 27 / 27 both | 39 / 131, 43 / 135 ms | 11 / 11 both | 3 % / 0 % |
| LAN babble -44, player -38 | LOW | 27 / 27 (always open) | 0 ms | 0 / 11 | 99 % |
| | AUTO (thr -38) | 23 / 27 | 46 / 137 ms | 11 / 11 | 0 % |
| breaths -50 between phrases, speech -40 | LOW | 33 / 33, breaths 0 / 6 | 50 / 135 ms | 12 / 12 | |
| | AUTO | 33 / 33, breaths 6 / 6 | 30 / 42 ms | 12 / 12 | |

- Floor step -73 -> -50 dBFS: AUTO follows in 4.0 s (gate open the whole time meanwhile; LOW is
  open 61 % of the time in that room anyway); back to -73: 0.34 s. Afterwards 10 / 10 words.
- Real, `owner_mictest_g50`: LOW cuts 30 windows (600 ms) of speech louder than -55 dBFS in 10 s
  (quiet syllables of real words, e.g. 140 ms in the middle of one); AUTO (threshold -60) cuts
  none. AUTO is open 92 % of the windows against 74 %: it stays open through short gaps and on
  two lip / breath sounds at -51 / -53 dBFS (each then held 200 ms).
- Real idle (`raw_idle.wav`, floor -74, peaks -63): threshold -60, never opens.
- Steady tone at -32 (`mic-tone`): after 3 s it is "the floor"; the threshold stops at -35 and
  the tone still passes. A fan or hum louder than -35 dBFS would keep the gate open, as with LOW.
- HIGH is in `eval.py`'s output: it misses about half the words of a -40 dBFS talker.

### MIC VOLUME AUTO (start at MIC VOLUME 5)

| Talker (active level) | Gate | Settle | Gain end / ideal | Moves in pauses | Spread, 2nd half (p-p) | Output active level |
|---|---|---|---|---|---|---|
| -52 | AUTO | 6.6 s | +15.0 (max) / +15.0 | 0.00 dB | 0.0 dB | -35 (out of range) |
| -45 | AUTO | 5.8 s | +15.0 / +15.0 | 0.00 | 0.0 | -28 |
| -40 | AUTO | 3.2 s | +10.2 / +10.0 | 0.00 | 0.45 | -28 |
| -35 | AUTO | 1.7 s | +5.7 / +5.0 | 0.00 | 0.45 | -27 |
| -25 | AUTO | 1.2 s | -3.8 / -5.0 | 0.00 | 0.23 | -27 |
| -18 | AUTO | 2.9 s | -10.6 / -12.0 | 0.00 | 0.26 | -27 |
| -40 | LOW | 4.0 s | +10.2 / +10.0 | 0.00 | 0.85 | -28 |
| -52 | LOW | never | +0.4 | 0.00 | 0.4 | -47: LOW never opens, so the gain stays frozen |

- Settle = from the first word until the gain stays within 2 dB of where it ends up.
- Synthetic output reads -27..-28 dBFS, the owner's real speech -30.1 (the target is calibrated
  on him: his speech level spreads more than the synthetic one).
- Real, `owner_mictest_g50` (active level -43.6 dBFS): AUTO gain +13.5 dB (shown as level 9),
  within 2 dB after 3.7 s, output active level -30.1 dBFS. `owner_mictest.wav` (headset mic gain
  100): +11.8 dB (level 9). So at MIC VOLUME 5 his teammates get him 12..14 dB under the -30
  dBFS that VoIP aims for.
- One 2.5 s shout (+12 dB): gain dip 0.0 dB (inside the dead band), level unchanged 3 s later.
- Talker 12 dB louder for good: the gain comes down 6 dB over the next 30 s and is still going
  (the clipped average follows a lasting change slowly: the price of ignoring shouts); the
  limiter holds the peaks meanwhile.
- Breaths at -48 dBFS between phrases (speech -40): +11.4 dB instead of +10.2.
- Babble -50 / -44 (speech -40): +5.9 / +3.9 dB instead of +10: in a loud room AUTO gave less
  gain, not more (the room adds to the measured level).
- Muted for half the run: the gain does not move (10.68 -> 10.68 dB).

### 24 -> 8 kHz (sine sweep 100 Hz .. 11.9 kHz, output power at 8 kHz incl. aliases)

| | 0.1-3.2 kHz | 3.4 kHz | 4.0 kHz | worst above 4.6 kHz | delay |
|---|---|---|---|---|---|
| FIR (new) | -0.00..+0.35 dB | -1.2 dB | -11.2 dB | -62.1 dB | 0.917 ms |
| `rs_push` (today) | -2.16..0.00 dB | -2.5 dB | -1.8 dB | -4.8 dB | ~0.04 ms |

Today, everything between 4 and 12 kHz the mic picks up (hiss, fricatives, the 125 Hz comb's
upper teeth) folds back into the voice band only ~5 dB down; the FIR takes it 62 dB down. The
design response (`design.py`) agrees: +0.00..+0.35 dB, -61.9 dB.

### Comb canceller

- `raw_idle.wav`: comb teeth 5-9 kHz from +12.6 dB above the local spectrum to -6.8 dB
  (gone), 3-5 kHz +4.0 -> -6.7 dB; total noise -71.5 -> -71.8 dBFS (the comb is a tiny part
  of it). `raw_idle_g50.wav`: +14.3 -> +1.9 dB.
- Speech with a moving pitch: -0.09 dB. A held 125 Hz buzz (worst case): -9 dB after 0.5 s,
  -23 dB after 2 s.
- The comb sits at -75 dBFS: under every gate threshold (so it is muted in pauses) and 40 dB
  under speech. No audible gain is expected; it is not worth its 3 KB of heap and the risk to
  voices near 125 / 250 Hz. Recommendation: do not wire it (or only under `XW_DEV`).

## 3. Cost

| Kernel | Per 24 kHz sample | Host (M-series, `--bench`) | Xenon estimate |
|---|---|---|---|
| FIR decimator | 7.7 multiplies (23 per output, symmetric), 15 halfword loads | 4-5 ns | ~40 ns |
| `rs_push` it replaces | 1 divide, + 1 multiply and 1 divide per output | 1.4-2.4 ns | ~15 ns |
| Floor + AGC | ~14 multiplies per 20 ms window (0.03 per sample) | 26 ns per window | < 0.5 us per window |
| Comb (optional) | 0 multiplies; 1 load, 1 store | 5.6 ns | ~45 ns |

A driver pass covers at most ~16 ms of mic (384 samples) per headset: the FIR ~15 us gross,
~10 us net of the `rs_push` it replaces; floor + AGC under 1 us; the comb +17 us. So about
+11 us per headset per pass with what should ship, +28 us with the comb (budget +100 us). The
Xenon column is an estimate (in-order core, taken as ~8x the host's time per sample): measure
`work_us` in XWDG with each control on and off in session S1. Added delay: the FIR's 0.917 ms
minus `rs_push`'s ~0.04 ms = **+0.88 ms** (budget 1 ms). Image: all kernels compile to 2.6 KB
of PPC code plus 288 bytes of tables (host `clang --target=powerpc-unknown-linux-gnu
-msoft-float -O2`, zero FP instructions in the disassembly); budget 5 KB with the glue.
As built (the toolchain's link map, `tools/size_report.py`): `micdsp_360.c` +5008 bytes
(code +4.6 KB, constants +0.4 KB), release image 0x84000 -> 0x85000; heap 1120 bytes while a
switch is on (+3088 for the comb, XW_DEV only).

## 4. Integration, as built

`xbox360/micdsp_360.c` (driver thread, like the rest of the mic path; its "MIC-AUTO" comment
is the reference). What differs from the plan this section used to hold:

- **Switches, all off at boot:** `gate_auto` (NOISE GATE AUTO offered; off, a stored gate 3
  runs LOW), `mic_auto` (MIC VOLUME AUTO offered; off, it is never in effect), `fir`, and
  `comb` (XW_DEV builds only). `auto_on` is the player's choice (`xd_set_mic_auto`).
  `xd_gate_auto_offered` / `xd_mic_auto_offered` are the switches, for the panel.
- **Memory:** one malloc (1120 bytes, the four pads' floor tracker, AGC and decimator) made
  when a switch goes on and freed when all are off; the comb's 3 KB only in XW_DEV builds,
  once switched on. So with every switch off nothing runs and nothing is allocated. A pad's
  state restarts in `xd_reset_pad`; hot reload's `xv_quiesce` (which sets `g_quiesced` and
  resets every pad) frees it all, and nothing is allocated again until `xv_resume`.
- **Hooks** are where 4.1 of the plan put them: comb in `mic_clean`, floor / AGC per window in
  `mic_analyse` (`ma_window`, after the gate's decision), `mic_gain_target(pad)` for the gain,
  the decimator in `mic_to_360` when the capture resampler is set for 24 -> 8 kHz (its reduced
  rates are 3 / 1), else `rs_push` (`fir_bypass` counts). Switching the decimator off resets
  `rs_mic`'s running state so no stale sample clicks.
- **`mic_gain_target(int pad)`**: a macro in `voice_priv.h` turns a padless call (today's
  `mictest_360.c`) into `mic_gain_target(-1)`, the first headset, until its owner passes the pad.
- **Size:** the per-window helpers are `MICAUTO_SLOW` (out of line, `minsize`) in the plugin;
  the per-sample decimator stays inline at -O2.
- **Status block** (magic 0x58574D41, 20 words, `xbox360/micauto.py`): magic, version;
  controls `gate_auto mic_auto fir comb auto_on`; state of `pad` (the first headset, 4 = none)
  `floor_rms thr_rms speech_rms agc_q12 agc_level agc_state speech_windows` (RMS in sample
  units like XWDG's `gate_rms`; `agc_state` bit 0 frozen = the last window left the gain
  where it was, bit 1 acquiring, bit 2 moving); counters `auto_opens fir_samples fir_bypass
  heap_bytes nomem`. XWDG is version 4 (same layout): gate 3 is AUTO.

Proof, on the host (`tests/test_micdsp360.c` builds the real `micdsp_360.c`): every scenario
of `vectors.h` bit-exact with the model through the real code; the manual settings give the
same 8 kHz output and the same XWDG numbers whatever the switches say, and the same as the
code before MIC-AUTO built from git (its header says how). On the console:
`xbox360/accept/micauto.py`.

### Level of the decimator against today's path

`rs_push` is a 3-sample box filter: 0 dB at DC, falling to -2.2 dB at 3.2 kHz. The decimator
is flat (+0.00 .. +0.35 dB). Through the whole path (host test, -12 dBFS sine, gate OFF, MIC
VOLUME 5), decimator minus `rs_push`: 100 Hz +0.01, 300 Hz +0.16, 500 Hz +0.35, 1 kHz +0.35,
1.5 kHz +0.53, 2 kHz +1.14, 2.5 kHz +1.28, 3 kHz +2.23, 3.2 kHz +2.16, 3.4 kHz +1.27 dB. On
the owner's recordings (integer model, gate OFF or LOW, MIC VOLUME 5) his speech comes out
+0.15 .. +0.30 dB louder (`owner_mictest_g50.wav` +0.30 / +0.29, `owner_mictest.wav`
+0.15 / +0.16), and the share of the output above 3.4 kHz (mostly hiss folded down from
4..12 kHz) drops by 7 .. 10 dB. So the level matches within 0.5 dB; the top of the voice
band is no longer rolled off.

## 5. Design note: 8 -> 48 kHz chat playback (coefficients only, not code)

The earpiece path (`rs_ear`, chat 8 kHz -> headset 48 kHz) has the same problem in reverse:
linear interpolation leaves images of the voice at 8 kHz +/- f. The mirror of the decimator:
zero-stuff by 6 and low-pass at 48 kHz, done polyphase (6 phases x 14 taps: 14 multiplies per
48 kHz output sample, no zeros multiplied). 84 taps, equiripple, passband 0..3.2 kHz
(-0.00..+0.43 dB), stopband from 4.6 kHz (the first image of 3.4 kHz) at least 61 dB down,
delay 0.865 ms, gain 6 (each phase sums to ~32768 in Q15). Phase p (0..5) of an output uses taps
p, p + 6, p + 12, ... `design.py --interp` regenerates and checks it.

```
static const s16 interp6[84] = {
	14, -167, -272, -432, -592, -711, -744, -655, -431, -88, 320, 713,
	993, 1070, 887, 443, -197, -906, -1514, -1843, -1758, -1203, -233, 980,
	2168, 3020, 3250, 2676, 1279, -756, -3050, -5075, -6250, -6050, -4122, -384,
	4938, 11309, 17961, 24011, 28612, 31096, 31096, 28612, 24011, 17961, 11309, 4938,
	-384, -4122, -6050, -6250, -5075, -3050, -756, 1279, 2676, 3250, 3020, 2168,
	980, -233, -1203, -1758, -1843, -1514, -906, -197, 443, 887, 1070, 993,
	713, 320, -88, -431, -655, -744, -711, -592, -432, -272, -167, 14,
};
```

## 6. Opinion: what to offer

- **NOISE GATE AUTO: offer it.** On the owner's own recording LOW cuts 600 ms of real speech in
  10 s and AUTO cuts none; synthetic soft starts open ~15 ms sooner (p95 45 against 139 ms);
  a quiet talker gets every word instead of half; in a loud room it shuts out babble that holds
  LOW open 99 % of the time. Its costs: breaths and lip sounds 20+ dB over a quiet floor open
  it (6 / 6 at -50 dBFS, against 0 / 6 for LOW), it misses some words when the room is within
  ~6 dB of the talker, and a louder room takes 4 s to learn. A good candidate for the default
  once the owner has heard it.
- **FIR: ship it** (behind its control at first). A pure quality fix, measured 57 dB better,
  +0.88 ms, cheap, and nothing to tune.
- **MIC VOLUME AUTO: good enough to offer as a choice, not as the default yet.** It does not
  pump (0.00 dB of movement in any pause, under 1.3 dB p-p in steady speech, a shout leaves it
  alone), settles in 1..7 s, and lands the owner's real voice at -30.1 dBFS. The doubts: it is
  calibrated on one talker's 10 s; it follows a lasting change slowly (6 dB in 30 s); it raises
  the hiss under the voice by the same +13 dB (still ~33 dB under his speech); and with gate LOW
  a quiet talker never opens the gate, so it cannot help him (pair it with gate AUTO). Worst
  case it sits at +15 dB, which is MIC VOLUME 10 with today's limiter. If the owner's listening
  test in S2 finds anything wrong, ship gate AUTO and the FIR and keep MIC VOLUME manual: the
  finding above (his voice reaches teammates ~13 dB under target) then argues for a higher
  default step rather than AUTO.
- **Comb: do not ship** (see above).

## 7. The gate after the owner's reading

The owner said gate LOW cut his voice too early. Three 30 s mic test takes of him reading two
sentences twice (MIC VOLUME 5, 24 kHz, after the DC removal): with the gate OFF his 20 ms
window RMS spreads -60..-30 dBFS, most -45..-35, a fifth under -45, loud syllables -32; the
room floor is -74. LOW at -45 with a 200 ms hold shut him 13 times in the reading, 7 of them
gaps of 20-120 ms (mid-sentence), and every opening started at full level.

What changed (`micdsp_360.c`, `voice_priv.h`): LOW / HIGH became four steps, 1..4 at -60
-55 -50 -45 dBFS (stored numbers 1, 2, 4, 5; OFF 0 and AUTO 3 unchanged; AUTO not in effect
runs step 2); hold 400 ms for every setting, AUTO included; a shut gate opens in the 1 ms chunk
in which the window's sum already guarantees the threshold (it used to open only at the end of
that window, losing the whole first window of each word); it opens over 2 ms instead of 10;
closing stays 10 ms (it happens only after the hold, in a pause, from room noise to silence).
No look-back delay: the mic path's delay is unchanged (0 ms added). A 20 ms look-back with
the old 10 ms opening lost about as much of each word's start (33 %) as opening early with
2 ms does without delay (34 %); a 5 ms delay on top saved 7 points more.

On the gate-OFF take (`tests/gate_take.h`: the take's window levels only, no audio). Speech:
windows at -58 dBFS or more; an utterance: speech windows less than 300 ms apart (14
utterances, 879 speech windows, 20 word starts after 100 ms of quiet). Sample-level model
(`micdsp.py`'s gate on the WAV itself; AUTO as its -60 clamp) / the real `micdsp_360.c` on the
window levels (`tests/test_micdsp360.c --gate`, AUTO with its floor tracker):

| Gate | Speech lost | Closures in utterances | Word start lost, first 20 / 40 ms | Open outside speech (of 10 s) |
|---|---|---|---|---|
| LOW before (-45, 200 ms) | 6.0 % / 6.0 % | 6 / 6 | 82 / 80 % | 0.6 s |
| step 1 (-60, 400 ms) | 0.5 % / 0.3 % | 0 / 0 | 14 / 6 % | 5.0 s |
| step 2 (-55, 400 ms) | 0.8 % / 0.7 % | 0 / 0 | 34 / 22 % (median 5 %) | 3.5 s |
| step 3 (-50, 400 ms) | - / 2.0 % | - / 1 | 53 / 34 % | 3.0 s |
| AUTO before (-60, 200 ms) | 1.4 % / 1.3 % | 1 / 1 | 48 / 27 % | 2.8 s |
| AUTO now (-60, 400 ms) | 0.5 % / 0.3 % | 0 / 0 | 14 / 6 % (median 0 %) | 5.0 s |
| HIGH before (-35) / step 4 (-45) | 66.6 / 3.9 % | 12 / 2 | 100 / 63 % | 0.0 / 2.6 s |

Word-start loss that remains is words whose first 20 ms lie under the threshold (soft
consonants): only a look-back longer than a window would pass them. "Open outside speech" is
the room at -74 dBFS passing after a phrase: inaudible at that level.

NOISE GATE AUTO's own numbers, re-checked (`eval.py gate` with the new gate; "pause open" is
the share of each pause the gate stays open, from 250 ms after the word):

("LOW now" is step 2, -55 dBFS; step 4 is the old table's HIGH column at -45.)

| Scene | LOW now | AUTO now |
|---|---|---|
| speech -40 over pink -73 | 31 / 31 words, pause open 25 %, noise 0 % | 31 / 31, 26 %, 0 % (thr -60) |
| quiet talker -50 over -73 | 31 / 31 (HIGH: 18 / 31) | 31 / 31 (thr -60) |
| speech -40 over pink -50 | open 100 % on noise | 31 / 31, noise 0 % (thr -44) |
| LAN babble -55 / -50, player -38 | open 100 % on babble | 27 / 27, babble 0 % / 0 % |
| LAN babble -44, player -38 | open 100 % | 24 / 27 words (was 23), babble 0 % (thr -38) |
| breaths -50 between phrases | 6 / 6 breaths open it | 6 / 6 (as before) |

- Clamp -60 (min): in his room the floor is -74.7, so floor + 9 = -65.7 and AUTO sits on -60
  (measured -59.9 on the console). On the OFF take -60 loses 0.3 % of speech; -55 (LOW) would
  lose 0.7 % and 22 % more of each word's start. Lower than -60 gains nothing measurable (no
  speech window under -60 is lost now) and reaches the headset's own idle noise (peaks -63).
  Stays.
- Clamp -35 (max): it is what lets AUTO shut out babble at -44 (thr -38); at -45 AUTO would
  be open on that babble 100 % of the time, like LOW. The price is a quiet talker's words in
  such a room (3 of 27 for a -38 talker; this owner talks 5-10 dB quieter). No recording of
  him in a loud room exists: stays until one does.
- Margin 9 dB and the smoothing: in a quiet room they do not matter (the clamp decides); the
  pink / babble numbers in section 1 still hold. Stay.
- Start: -45 (= step 4), unchanged: started at -55 (step 2), AUTO was open 82 % of the first 4 s on a -50 dBFS room; in a quiet room the start does not matter (the floor drops at the first window).

Cost: no added delay. Per 1 ms chunk one 64-bit compare more; per sample the gate's step is
now computed only while it moves (it used to be every sample). Host timing of the whole
`--gate` path: old and new both 0.02 s for 120 s of mic (no difference to measure).

### The mic test plays through the gate

The mic test records the mic before MIC VOLUME and the gate (after the DC removal) and its
playback applies them, 1 ms at a time, with the live path's own steps (`voice_priv.h` gate_*,
`shape()`) at the threshold and volume in effect while it plays (AUTO: its live threshold of
the moment, not re-learnt from the clip). A player records a sentence once and steps NOISE
GATE (one row up from MIC TEST) while it plays, hearing each step on the same words.
`micdsp.playback` is the same in Python (bit-exact: `MV_PLAY_HASH`, `tests/test_micdsp360.c`);
`voice.py mictest save --gated` uses it. Cost: the gate and the volume now run on the clip
while it plays instead of while it records (the same work, moved); no second buffer.
