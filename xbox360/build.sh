#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Xbox 360 side with OpenXeChain. Run in this folder on the toolchain host:
#   OXC=/path/to/openxechain/sysroot bash build.sh
#
#   out/xa.xex         the driver (system DLL at 0x91500000)
#   out/xa_loader.xex  development loader for it
#   out/xa_m3.xex      a register probe for bring-up (system DLL at 0x91D00000)
#   out/xw.xex         wired controllers (system DLL at 0x91580000)
#   out/xw_loader.xex  development loader for it
#   out/voicetest.xex  XamVoice* round-trip test title (voice.py test)
#   out/echo.xex       record / play / echo headset test title (echo.py)
#
# Switches for xw.xex (environment; docs/DEVELOPMENT.md):
#   XW_DEV=1   development build: -DXW_DEV (probes, traces, dummy loads, verbose logs).
#              Default 0, the release build.
#   XW_TEAM=1  link the real team voice (team_360.c). Default 0: the stub, because
#              team voice is not verified on two consoles and is not published.
set -euo pipefail
cd "$(dirname "$0")"
TC=${OXC:-/opt/openxechain/sysroot}
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir -p out
# System DLLs: 4 KB pages (/align:0x1000) and SynthXEX's system-DLL patch (README, Building).
# Never -g (the toolchain's clang crashes emitting DWARF into COFF). lld's
# "/align specified without /driver" warning is expected.
DLL_LINK="-Wl,/entry:DllMain,/align:0x1000"
# SynthXEX maps only the bytes the PE file holds, so zero-initialised data (.bss) would
# lie outside the loaded image. Keep it in .data; check_xex.py fails the build otherwise.
# No floating point: DLL code runs on the kernel's DPCs, which have no FP context, and
# the ppc32 ABI's variadic prologue spills f1-f8 unconditionally (stfd) in every printf-
# style function. check_xex.py fails the build on any FP instruction.
CFLAGS_DLL="-fno-zero-initialized-in-bss -msoft-float"
# XA_HOOKS picks the installed XInput hooks (default: read,caps,set; "read" = input only)
case "${XA_HOOKS:-read,caps,set}" in
    *caps*) CFLAGS_DLL="$CFLAGS_DLL -DXA_HOOK_CAPS=1" ;;
esac
case "${XA_HOOKS:-read,caps,set}" in
    *set*) CFLAGS_DLL="$CFLAGS_DLL -DXA_HOOK_SET=1" ;;
esac

# XA_SLOT=n: build the plugin as out/xa<n>.xex at its own base, and the loader for it, so
# a new build can replace a running one without a reboot (xa.py reload). A loaded
# module's name and address can't be reused until the console reboots. Slot 0 (default)
# is xa.xex at 0x91500000; slot n is xa<n>.xex at 0x92000000 + (n-1) MB.
#
# A system DLL only loads at its own base, so it must not overlap another plugin's.
# 0x91B00000-0x91E70000 is crowded (RPC360 and others sit at 0x91E00000, connectx at
# 0x91E60000); the gap between hud.xex (ends 0x91429000) and ximecore.xex (0x91680000)
# is unused by system modules and known plugins.
SLOT=${XA_SLOT:-0}
if [ "$SLOT" = 0 ]; then
    NAME=xa
    BASE=0x91500000
else
    NAME=xa$SLOT
    BASE=$(printf "0x%08X" $((0x92000000 + (SLOT - 1) * 0x100000)))
fi

# micro-ecc: only P-256, portable C (no assembly)
ECC_FLAGS="-DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0
    -DuECC_SUPPORTS_secp256k1=0 -DuECC_PLATFORM=uECC_arch_other"
CORE="../core/mt76.c ../core/dongle.c ../core/gip.c ../core/sha256.c ../third_party/micro-ecc/uECC.c"

$TC/bin/clang -O2 -Wall -Wno-unused-parameter $CFLAGS_DLL $ECC_FLAGS $DLL_LINK,/base:$BASE \
    -DXA_NAME="\"$NAME.xex\"" \
    thunks.s xa_plugin.c platform_360.c xinput_360.c detour.c $CORE -o out/$NAME.exe
$TC/bin/synthxex-patched -i out/$NAME.exe -o out/$NAME.xex -t sysdll >/dev/null

# xw.xex: wired Xbox One / Series controllers (xw_plugin.c). Its own base, right after
# xa.xex's slot, so both can be installed (load only one of them at a time: they hook the
# same XInput functions).
XW_BASE=0x91580000
# the on-screen headset bar: hud_360.c when it is there, else the do-nothing stub
XW_HUD=hud_360_stub.c
[ -f hud_360.c ] && XW_HUD=hud_360.c
# console-to-console team voice (team_360.h): the stub unless XW_TEAM=1 (see the top)
case "${XW_TEAM:-0}" in
    0) XW_TEAM_SRC=team_360_stub.c ;;
    1) XW_TEAM_SRC=team_360.c ;;
    *) echo "XW_TEAM: 0 (stub, default) or 1"; exit 1 ;;
esac
case "${XW_DEV:-0}" in
    0) XW_FLAGS="" ;;
    1) XW_FLAGS="-DXW_DEV" ;;
    *) echo "XW_DEV: 0 (release, default) or 1"; exit 1 ;;
esac
# everything in xw.xex but the thunks. Feature modules with no implementation yet are stubs
# that return the defaults (eq_360.c, surround_360.c, settings_360.c, file_360.c).
# Wireless (docs/WIRELESS-NOTES.md): $CORE already brings mt76.c and dongle.c into xw.xex;
# one plugin for both would add xa_plugin.c's adapter match, firmware load and dongle ops here,
# sharing xinput_360.c's 4 slots. Never load xw.xex and xa.xex together (same XInput hooks).
# reload_360.c and selftest_360.c are always listed: their contents are #ifdef XW_DEV.
# net_360.c (UDP as XAM's system caller) serves the PC bridge (bridge_360.c) and team voice.
XW_SRC="xw_plugin.c reload_360.c selftest_360.c platform_360.c xinput_360.c rumble_360.c
    voice_360.c side_360.c micdsp_360.c mictest_360.c game_360.c panel_360.c hud_panel.c
    $XW_HUD $XW_TEAM_SRC eq_360.c surround_360.c settings_360.c file_360.c detour.c
    net_360.c bridge_360.c"
echo "xw.xex: team voice $XW_TEAM_SRC, ${XW_FLAGS:-release}"
$TC/bin/clang -O2 -Wall -Wno-unused-parameter $CFLAGS_DLL $ECC_FLAGS $DLL_LINK,/base:$XW_BASE \
    -DXA_NAME="\"xw.xex\"" -DXA_WIRED=1 -DXA_HOOK_STATS=1 ${XW_STAGED:+-DXW_STAGED=1} $XW_FLAGS \
    thunks_wired.s $XW_SRC $CORE -o out/xw.exe
$TC/bin/synthxex-patched -i out/xw.exe -o out/xw.xex -t sysdll >/dev/null

# XW_SLOT=n (n >= 1): also build the same source as out/xw<n>.xex, under its own name and at
# its own base, with a loader for it, for a hot reload (xw.py reload, docs/HOT-RELOAD.md): a
# loaded module's name and address can't be reused until the console reboots, and the build
# it replaces stays loaded. A slot build waits for xw.py to say what it replaces before it
# hooks anything, so it is never a boot plugin. Slot n sits at 0x92800000 + (n-1) MB (the
# image is under 1 MB; xa.xex's own slots start at 0x92000000, and the two plugins are never
# loaded together), up to slot 24.
XW_SLOT=${XW_SLOT:-0}
if [ "$XW_SLOT" != 0 ]; then
    case "$XW_SLOT" in
        *[!0-9]*) echo "XW_SLOT must be a number"; exit 1 ;;
    esac
    [ "$XW_SLOT" -le 24 ] || { echo "XW_SLOT: 1..24"; exit 1; }
    XWS=xw$XW_SLOT
    XWS_BASE=$(printf "0x%08X" $((0x92800000 + (XW_SLOT - 1) * 0x100000)))
    $TC/bin/clang -O2 -Wall -Wno-unused-parameter $CFLAGS_DLL $ECC_FLAGS $DLL_LINK,/base:$XWS_BASE \
        -DXA_NAME="\"$XWS.xex\"" -DXW_SLOT=$XW_SLOT -DXA_WIRED=1 -DXA_HOOK_STATS=1 $XW_FLAGS \
        thunks_wired.s $XW_SRC $CORE -o out/$XWS.exe
    $TC/bin/synthxex-patched -i out/$XWS.exe -o out/$XWS.xex -t sysdll >/dev/null
    [ $(($(stat -c %s out/$XWS.exe))) -lt $((0x100000)) ] || { echo "$XWS.exe no longer fits its 1 MB slot"; exit 1; }
    # its loaders (xw.py reload --loader): the plugin next to the loader on the USB stick, or
    # in Hdd:\Plugins with Aurora in Hdd:\Aurora
    $TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry -DPLUGIN_NAME="\"$XWS.xex\"" \
        -DPLUGIN_DIR="\"xwired\"" -o out/xw_slot_loader.exe
    $TC/bin/synthxex -i out/xw_slot_loader.exe -o out/xw_slot_loader.xex -t title >/dev/null
    $TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry \
        -DPLUGIN_PATH="\"\\\\Device\\\\Harddisk0\\\\Partition1\\\\Plugins\\\\$XWS.xex\"" \
        -DAURORA_DIR='"\\Device\\Harddisk0\\Partition1\\Aurora"' -o out/xw_slot_loader_hdd.exe
    $TC/bin/synthxex -i out/xw_slot_loader_hdd.exe -o out/xw_slot_loader_hdd.xex -t title >/dev/null
    python3 check_xex.py out/xw_slot_loader.exe out/xw_slot_loader.xex
    python3 check_xex.py out/xw_slot_loader_hdd.exe out/xw_slot_loader_hdd.xex
    python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/$XWS.exe out/$XWS.xex
    echo "$XWS" > out/xw_built_slot # read by xw.py reload
fi

$TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry -DPLUGIN_NAME="\"xw.xex\"" -DPLUGIN_DIR="\"xwired\"" \
    -o out/xw_loader.exe
$TC/bin/synthxex -i out/xw_loader.exe -o out/xw_loader.xex -t title >/dev/null
# the same loader for a console with the plugin in Hdd:\Plugins and Aurora in Hdd:\Aurora
$TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry \
    -DPLUGIN_PATH='"\\Device\\Harddisk0\\Partition1\\Plugins\\xw.xex"' \
    -DAURORA_DIR='"\\Device\\Harddisk0\\Partition1\\Aurora"' -o out/xw_loader_hdd.exe
$TC/bin/synthxex -i out/xw_loader_hdd.exe -o out/xw_loader_hdd.xex -t title >/dev/null

# voicetest.xex: a title that runs a XamVoice* round trip through the virtual headset
# (voice.py test)
$TC/bin/clang -O2 -Wall -fno-zero-initialized-in-bss voicetest.c -Wl,/entry:vt_entry \
    -o out/voicetest.exe
$TC/bin/synthxex -i out/voicetest.exe -o out/voicetest.xex -t title >/dev/null

# echo.xex: a headset test title (record / play / delayed echo through XamVoice*; echo.py)
$TC/bin/clang -O2 -Wall -fno-zero-initialized-in-bss echo.c -Wl,/entry:ec_entry \
    -o out/echo.exe
$TC/bin/synthxex -i out/echo.exe -o out/echo.xex -t title >/dev/null

$TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry -DPLUGIN_NAME="\"$NAME.xex\"" \
    -o out/xa_loader.exe
$TC/bin/synthxex -i out/xa_loader.exe -o out/xa_loader.xex -t title >/dev/null

$TC/bin/clang++ -O2 -fno-exceptions -fno-rtti $CFLAGS_DLL $DLL_LINK,/base:0x91D00000 \
    m3_probe.cpp -o out/xa_m3.exe
$TC/bin/synthxex-patched -i out/xa_m3.exe -o out/xa_m3.xex -t sysdll >/dev/null

python3 check_xex.py out/xa_loader.exe out/xa_loader.xex
python3 check_xex.py out/xw_loader.exe out/xw_loader.xex
python3 check_xex.py out/xw_loader_hdd.exe out/xw_loader_hdd.xex
python3 check_xex.py out/voicetest.exe out/voicetest.xex
python3 check_xex.py out/echo.exe out/echo.xex
echo "$NAME" > out/built_name # read by xa.py (upload, load, reload)
for x in $NAME xw xa_m3; do
    python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/$x.exe out/$x.xex
done
ls -l out/*.xex
