# AmpJack

Wired Xbox One / Series controllers on a modded Xbox 360. Plug a headset into the controller and skip the MixAmp.

![The panel over Halo 3](docs/img/panel.png)

Hold **Share** for the panel. Tap **Share** to mute.

## Install

1. Download the zip from the [latest release](https://github.com/Roasted-Codes/AmpJack/releases/latest).
2. Copy `xw.xex`, `xw.presets` and `xw.demo` to `Hdd:\Plugins\`.
3. Add `xw.xex` to a free plugin slot in `launch.ini`.
4. Reboot.

Needs kernel 17559. More in [`docs/WIRED.md`](docs/WIRED.md).

## Thanks

- [Jacckii](https://github.com/Jacckii/Xbox360-Series-Controllers-Native): the plugin this is forked from.
- [xone](https://github.com/dlundqvist/xone) (medusalix and contributors): controller protocol.
- [mt76](https://github.com/openwrt/mt76) (Felix Fietkau, Lorenzo Bianconi, Stanislaw Gruszka): wireless adapter definitions.
- [hiddriver360](https://github.com/EinTim23/hiddriver360) (EinTim23): USB and XInput route.
- [The ZEFERENCE](https://github.com/XxUnkn0wnxX/TheZEFERENCE) (ZaliaS): sound presets.
- [SADIE II](https://www.york.ac.uk/sadie-project/database.html) (University of York, Apache-2.0): surround data. See [`NOTICE`](NOTICE).
- iMoD1998: Detours.h.
- [micro-ecc](https://github.com/kmackay/micro-ecc) (Kenneth MacKay): cryptography.
- [OpenXeChain](https://github.com/OpenXeChain) and SynthXEX: toolchain.
- [BadUpdate](https://github.com/grimdoomer/Xbox360BadUpdate) (grimdoomer): code on kernel 17559.

[GPL-3.0](LICENSE).
