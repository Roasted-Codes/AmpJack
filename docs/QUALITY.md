# Quality tools

Three tools that keep the plugin small, the refactors honest and the console's cost
measured. They are Python only and change nothing on the console.

| Tool | Question it answers | Needs |
|---|---|---|
| `tools/size_report.py` | Where do the image's bytes go? Did this change make it bigger? | Docker (the toolchain image) |
| `tools/fn_diff.py` | Does this refactor change what any function does? | Docker, two builds with their maps |
| `xbox360/accept/overhead.py` | What does the plugin cost the console while it runs? | the console (`XBOX=...`), or `--dry-run` |

## Size report

```bash
bash toolchain/build.sh
python3 tools/size_report.py                      # image, code / constants / data, per file, top 25 symbols
python3 tools/size_report.py --save size.json     # keep it as a baseline
python3 tools/size_report.py --diff size.json     # after a change: what grew or shrank
```

- "code" is `.text`, "constants" `.rdata` (strings, tables), "data" `.data`. Data includes
  every zero-filled static buffer, because the plugin has no `.bss` (`docs/INTERNALS.md`), so
  a static buffer costs image bytes; malloc it at init instead.
- The PE has no symbols, so the names come from lld-link's link map. `build.sh` does not
  write one, so the tool rebuilds a throw-away copy of the tree in the toolchain container
  with the map switched on (about a minute), checks the result is byte-identical to
  `xbox360/out/xw.exe`, and keeps the map as `out/xw.lldmap`. If the exe is older than the
  source it refuses: run `bash toolchain/build.sh` first.
- A symbol's size runs to the next symbol, so it includes alignment padding. String literals
  are summed per file as `(strings)`; import thunks and tables as `(linker)`.

## Proving a refactor changes nothing

```bash
python3 tools/size_report.py --snapshot /tmp/before   # xw.exe + its map, before the change
# ... move code, split files, fence blocks; bash toolchain/build.sh ...
python3 tools/fn_diff.py /tmp/before/xw.exe xbox360/out/xw.exe
```

It disassembles both builds and compares every function after replacing what moving code
changes: call and branch targets become names, addresses built with `lis` and a low half
become the name of what they point at, string literals become their text. It also compares
named constants and initialised data. The first lines are the verdict:

```
functions: 395 identical, 0 changed, 0 added, 0 removed (4 of the identical ones at another address)
objects (named constants and data): 382 identical, 0 changed, 0 added, 0 removed
```

Then come the functions that moved file, and a short diff for each changed one. The exit
status is 1 if anything changed.

- **Moving a function to another file is not always a no-op for the compiler.** A function
  that was inlined into its callers in the old file becomes a call (and the other way
  round). `fn_diff` reports those callers as changed; their diffs show calls appearing or
  disappearing. That is a real change in the machine code (usually harmless), so it is named
  in the review, not hidden.
- A changed function whose only difference is a raw number next to a register that holds an
  address (for example `lwz 3, -11264(29)` against `lwz 3, -15360(29)`) means the tool lost
  track of that address; check it by eye. This did not happen on the test builds.
- `--lines N` lengthens the diffs; `--all` also lists the identical functions.

## Overhead scorecard

```bash
XBOX=10.0.0.13 python3 xbox360/accept/overhead.py --seconds 900 --save overhead-base.json
XBOX=10.0.0.13 python3 xbox360/accept/overhead.py --seconds 900 --compare overhead-base.json
python3 xbox360/accept/overhead.py --dry-run      # no console: a canned reading
```

It reads the counters every 5 s over the window and prints one table: the image size of the
local build, driver-thread work per pass, passes by length, earpiece and game-audio dropouts,
mic packets per second, and the panel's draw time and the frame rate. The blocks are read by
`xw.py`, `voice.py` and `hud.py`; a block the running build lacks shows as `-` with the
reason. With `--compare` it prints one PASS / FAIL line per target below.

### "Low overhead", as numbers

In one line for the owner: how big it is, how much memory it takes, how much of the
console's time it uses, how much later sound arrives, and whether anything ever drops out.
Report the table once on today's build (baseline) and once on the release build, with one
row per feature showing its change when it is switched on.

| Measure | How it is read | Target for release |
|---|---|---|
| Plugin size | build output (`size_report.py`, `check_xex.py`) | below the baseline with every new feature in; hard limit 0x100000 |
| Extra memory while running | the modules' allocation counters (a `heap_bytes` word per block, when added) | under 400 KB idle; +1.44 MB only while a mic-test clip is held |
| Driver-thread work per pass | XWDG `work_us`, `work_max_us`, `pass_hist` | average rise under 0.5 ms with everything on; no new passes of 25 ms or more |
| Game-audio thread time | XWSR `tap_us` p99 / max (when the surround block exists) | under 300 us p99 of a 5.33 ms frame with surround on |
| Picture cost | XH36 draw time, `swaps_per_s`, `pres_pct` | frame rate unchanged; panel draw under 1 ms; mute mark under 0.25 ms |
| Added sound delay | computed: game buffer 45 ms, EQ 0, surround under 1.4 ms, mic filter under 1 ms | no more than +2.5 ms in total |
| Dropouts over a 15-minute match | XWDG `out_starved`, `out_frames_missed`; XVGA `underruns`; XWAU `mic_pps` | 0 / 0 / 0 / 1000, as today |

`overhead.py` reads every measure that has a counter today. Memory and game-audio thread time
get rows when their counters exist; add them to `flat()`, `ROWS` and `TARGETS` in the script
(the block's own tool keeps its layout). Sound delay is computed from the settings, not read.

### Baseline numbers (the build at the start of the block)

Image 0xde000 (909,312 bytes): code 245,840, constants 23,276, data 627,740. Biggest symbols
(all zero-filled data): `rx_ring` 131,072 (`platform_360.c`), `g_micdump` 65,556
(`xw_plugin.c`), `g_tk` 37,044 (`team_360.c`), `g_ports` 35,840, `rx_frame` 33,900,
`process_wlan.frame` 33,792 (`core/dongle.c`), `g_ring` 32,856 (`game_360.c`), `xa_log_ring`
32,784, the two HUD canvases 29,768 and 29,764.
