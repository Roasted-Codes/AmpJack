# Wired Xbox One / Series controllers (xw.xex)

`xw.xex` drives wired Xbox One and Series controllers (any USB pad whose first interface is
GIP, class/subclass/protocol `FF/47/D0`: Microsoft, SCUF, PowerA, PDP, 8BitDo and the rest of
xone's wired list) straight from the 360's USB ports. No adapter, no dongle. Up to four, as
standard XInput controllers, alongside the console's own 360 controllers.

**Status:** works on the console with a Microsoft Xbox Series pad (`045e:0b12`) and a headset
in its jack. The SCUF Instinct Pro (`2e95:0504`) is verified with the Mac test tool
(`desktop/xwired`) only. The headset features are described in `docs/HANDOFF.md`.

## Requirements

- Kernel **17559** with a plugin loader (BadUpdate + XeUnshackle, or an RGH with DashLaunch).
- A USB **data** cable (charge-only cables carry no data).
- Don't load `xa.xex` or hiddriver360 at the same time: all three hook the same XInput
  functions.

## Install

No computer is needed once the file is on the console.

1. Download `xw.xex` from the [latest release](https://github.com/Roasted-Codes/Xbox360-GIP-Wired/releases/latest)
   (or build it: `bash toolchain/build.sh`).
2. Copy it to the 360's hard drive, e.g. `Hdd:\Plugins\xw.xex` (USB stick + Aurora's file
   manager, or FTP).
3. Add it to a free plugin slot in `launch.ini` (DashLaunch can set this on the console):

   ```ini
   [Plugins]
   plugin3 = Hdd:\Plugins\xw.xex
   ```

4. Reboot. Plug the controller in and press its Xbox button; plug the headset into the
   controller. Hold **Share** for the headset panel; tap Share to mute.

## How it works

| Step | Where |
|---|---|
| A class driver in the kernel's USB driver list accepts interface 0 = `FF/47/D0` | `XwMatchC` |
| AddDevice opens the interrupt IN/OUT pair (`UsbdGetEndpointDescriptor`, ord. 744) | `XwAddDeviceC` |
| Two IN transfers per controller are always queued; each completion copies the packet into a per-controller ring and re-queues | `XwInDoneC` |
| OUT packets go through a per-controller FIFO, one transfer in flight | `wired_send`, `XwOutDoneC` |
| The driver thread runs `core/gip.c` per controller: announce → identify → power on → LED → input | `service_port` |
| No security handshake (`gip_transport.no_auth`): on a cable the controller reports input once powered on, as xpad drives it | `core/gip.c` |
| A controller that never announces itself is started anyway after 1.5 s | `gip_poll` |
| Input, rumble, Guide → `xinput_360.c`, unchanged from `xa.xex` | |
| Controllers already plugged in at boot are re-offered after the title switch, as `xa.xex` does for the adapter | `grab_present_controllers` |

Unknowns the first console test answers (all logged, `xw.py status` shows them):

- Whether Usbd offers a composite device's interfaces one by one to an interface-level
  driver, as the console's own wireless driver gets them (`match calls`, `adds`).
- What Usbd leaves in an IN block's length field after a short packet (`IN length field after
  completion`). The driver doesn't depend on it: it zeroes each IN buffer before queueing, and
  GIP parses the zero padding as empty.
- Whether a given third-party pad needs extra start-up packets (xpad has a quirk table for a
  few PDP/Hori/PowerA models). The log shows whether it announced itself and what it sent.

## Test on a Mac or PC first

`desktop/xwired` runs the same GIP code over libusb and prints the controller's input. It
proves a controller works with this driver before going near the console.

```bash
brew install libusb            # macOS; on Linux: libusb-1.0-0-dev
make -C desktop
./desktop/xwired               # sudo ./desktop/xwired if it can't claim the controller
XA_DEBUG=3 ./desktop/xwired    # every USB packet in and out
```

Plug the controller **directly** into the computer (not through an OGX-Mini or other adapter).
Buttons and sticks print as you move them; A/B and the triggers drive the rumble motors.

## Build

The toolchain (OpenXeChain) builds in a container, so nothing is installed on the host:

```bash
bash toolchain/build.sh        # first run builds the toolchain image (about an hour)
```

Outputs `xbox360/out/xw.xex` (system DLL at `0x91580000`) and `xbox360/out/xw_loader.xex`, next
to the original `xa.xex`.

## Debug on the console (xbdm)

```bash
cd xbox360
export XBOX=192.168.1.50       # the console's IP
python3 xw.py upload           # to Usb0:\Apps\xwired
python3 xw.py load             # load without a reboot (then it launches Aurora)
python3 xw.py watch            # follow the log and the controllers
python3 xw.py debug 1          # log every GIP message (2: input too)
python3 xw.py boot on          # load at every boot
python3 xw.py reload           # replace the running build, no reboot: docs/HOT-RELOAD.md
```
