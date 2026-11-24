# Headset presets (`xw.presets`)

What the PRESET row on the panel offers, how it is built, and why the curves are what they
are. Build: `python3 tools/presets/convert.py` (writes `xw.presets` here and in
`xbox360/out/`), install: `python3 xbox360/eq.py install`.

## What ships, and in what order

`names.txt` is the list (panel order, short name, the one-line description the panel shows).
The owner's final list. After FLAT: HALO MCC, OMNIVOX, ARCTURUS, DYSTRILATION, PURE, MOVIE:
INCENDIARY, BASS BOOST, REVIVE A40 GEN3, REVIVE A40 GEN4, then CUSTOM.

- From TheZEFERENCE (`upstream/`, a complete unchanged mirror; author ZaliaS, credit in
  `upstream/CREDITS.txt`): HALO MCC, five general-purpose ones (INCENDIARY is shown as
  "MOVIE: INCENDIARY" so a player can find the film one; the name and the ZaliaS credit stay),
  and the two A40 retunes (only for that exact headset, and the descriptions say so).
- This project's own (`xw/*.xweq`, author "Xbox360-GIP"): BASS BOOST. `xw/bass-boost.xweq`
  explains the format.
- Not shipped, on purpose: the `DROPPED` lines at the end of `names.txt` (games that never
  came to the 360 or the original Xbox, the wireless A50 retunes, near-duplicates, and the
  ones the owner left out). A saved choice of a dropped preset loads as FLAT.

This project's HALO 2 and HALO 3 presets were first guesses made without measurements and were
dropped by the owner, with VOICES, LATE NIGHT and the two STEPS band-passes (git history has
their files).

The descriptions say what a player hears, never numbers. The upstream presets were made for
Astro headsets, which are dark; on another headset they all lean bright, and the descriptions
describe them as they play here (`convert.py` prints each curve and its description).

A description is what the panel's help line says while the cursor is on PRESET: one line of
at most 36 characters (`convert.py` `HELP_COLS` refuses a longer one), so a thing it heard and
its trade-off, e.g. `MORE THUMP. HARDER TO HEAR STEPS`. FLAT's and CUSTOM's are in
`xbox360/eq_360.c`, held to the same 36. The file keeps its 64-byte field (no format change);
a build that meets a longer text from an older file cuts it after the last whole word that
fits.

## The file (version 2)

`xbox360/eq_360.h` has the layout: header, one 224-byte record per preset (five biquads, a
preamp, the name, the author, the description), then CUSTOM's tables. A version-1 file (the
one on the console before this build) is refused cleanly: FLAT only, one log line, XWEQ
`load_error` -19; `eq.py install` puts the new one in place. A wrong CUSTOM table costs CUSTOM
only, never the presets.

## Loudness

Every preset plays exactly as loud as FLAT on K-weighted pink noise (`convert.py` LOUDNESS):
switching never jumps. That level is the quietest preset at its clip-safe preamp (today BASS
BOOST): -5.3 dB (VOLUME on the panel buys it back). Two exceptions, both left out of that
"quietest" choice so they cannot drag every other preset down:

- `Level = own` in an `.xweq` (meant for a band-pass; none ships): matched downward when it
  can be, otherwise as loud as it can be without going over 0 dB, so possibly quieter.
- CUSTOM: see below.

## CUSTOM

Three plain controls for a player who "doesn't speak kHz", each -6..+6 steps of 1.5 dB:
BASS is a low shelf (half its effect at 200 Hz: rumble, booms, the weight of shots), MIDS a
broad bell at 1 kHz (about two octaves: voices and the body of most sounds), TREBLE a high
shelf (half at 4 kHz: footsteps' crunch, clicks, sparkle). Gentle shapes on purpose: no
setting can sound broken, every step is audible. All three at 0 is FLAT itself, bit for bit.

The console has no floating point, so `custom.py` designs every step of every control and the
preamp of every one of the 2197 combinations (tabled: the level of a combination is not the
sum of its parts, a formula was up to 3.3 dB off). Each combination is as loud as FLAT when it
can be; one that boosts must turn down by its peak to stay under 0 dB, so 505 of them are
quieter, by at most 3.8 dB. The generator checks every combination's peak and level and proves
no internal number can overflow for any of them.

Each change of a control crossfades like a preset change (no click); holding a direction
(8 steps a second) is safe. The steps and "CUSTOM is selected" are saved per profile.
