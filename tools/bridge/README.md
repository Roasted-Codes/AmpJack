# Xbox headset bridge for a Linux PC

Use the headset on your Xbox controller for Discord (or any app) on a Linux PC, and let the
people on Discord and the Xbox players hear each other.

## What you need

- The Xbox with this `xw.xex` running, on the same network as the PC.
- A Linux PC with normal desktop sound (PipeWire or PulseAudio; almost every desktop has one).
  Nothing to install: the program needs only Python 3 and the sound tools that come with it
  (`pactl`, `parec`, `pacat`). If it says one is missing, install `pulseaudio-utils`.
- Nothing to set on the Xbox. It waits for the PC by itself.

## Start it

Copy `xbox-bridge.py` to the PC and run, with the Xbox's address:

```
python3 xbox-bridge.py 10.0.0.13 --bridge
```

It prints `Xbox connected.` once the Xbox answers. Leave the window open while you play.
Press **Ctrl+C** to stop; the extra sound devices go away again.

## Set up Discord (once)

User Settings > Voice & Video:

- **Input Device: Xbox Bridge Mic** (your headset mic, plus the Xbox players, so Discord hears both)
- **Output Device: Xbox Bridge** (you hear Discord in the headset, and the Xbox players hear it too)

That is all. In the Xbox game, chat works as before.

## Without `--bridge`

`python3 xbox-bridge.py 10.0.0.13` makes four separate devices, for choosing yourself:

| Device | Kind | What it does |
|---|---|---|
| Xbox Headset | output | what plays here goes into your headset (music, Discord) |
| Xbox Chat (to Xbox players) | output | what plays here, the Xbox players hear |
| Xbox Headset Mic | input | your headset's microphone |
| Xbox Players | input | what the Xbox players say |

`--bridge` adds **Xbox Bridge** and **Xbox Bridge Mic** on top. Devices named "Xbox bridge
internal" are its plumbing: don't pick those.

## If something is off

- **"Waiting for the Xbox"** and nothing else: check the address (Aurora shows it), that the
  PC and the Xbox are on the same network, and that a firewall on the PC lets the program get
  answers (it uses UDP port 3670 on the Xbox).
- **"headset NOT streaming"**: press the controller's Xbox button and check the headset is
  plugged in.
- **The Xbox players are too loud or quiet on Discord, or Discord too loud in the headset**:
  change the volume of that device in your PC's sound settings, or of the person in Discord.
- Your controller's mute (tap Share) mutes your mic for Discord too. The Discord voices still
  reach the Xbox players while you are muted.

## Checking it from a Mac or another PC (no sound devices)

```
python3 xbox-bridge.py 10.0.0.13 --test
```

sends test tones to the Xbox for 10 seconds, saves what the Xbox sends to two WAV files and
prints what arrived and what got lost. The console side has `xbox360/bridge.py status` and
the acceptance script `xbox360/accept/bridge.py`.
