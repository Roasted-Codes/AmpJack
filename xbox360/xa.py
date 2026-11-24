#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test helper for the xa plugin over xbdm (port 730). Run next to out/ (on the toolchain host).

  xa.py upload [FIRMWARE]   copy out/xa.xex, out/xa_loader.xex and the firmware to Usb0:\\Apps\\xadapter
                            (a file named xone_dongle_<pid>.bin keeps its name, others become 02fe)
  xa.py load                start the loader (it loads xa.xex as a system DLL)
  xa.py boot on|off         load xa.xex at every boot: a pluginN line in Usb0:\\launch.ini
                            (XeUnshackle's launch.xex); the old file is backed up first
  xa.py reload              build the next slot (xa<n>.xex, own base), have the running build
                            release the adapter, load the new one and hand the adapter over:
                            a new build with no reboot and no replug
  xa.py aurora              go back to Aurora
  xa.py watch [SECONDS]     follow the plugin's log and pad states
  xa.py status              one status dump
  xa.py reclaim             re-enumerate the adapter held by the xa_m3 probe, so xa gets it
  xa.py pair                turn pairing on
  xa.py unload              tear down and unload the plugin (then reload without a reboot)
  xa.py hold 0|1            debug: hold a USB removal before each step (off at load)
  xa.py step                debug: run the next held removal step
  xa.py led 0|1             test: send LED commands (1) or skip them (0); read at adapter start
  xa.py reg ADDR [VALUE]    debug: read (or write) an adapter register, e.g. reg 0x238
                            (add 0x40000000 for the CFG space, as MT_VEND_TYPE_CFG)

XBOX=host[:port] is the console's address (required).
"""
import os
import struct
import sys
import time

B = "\\"
FOLDER = "Usb0:" + B + "Apps" + B + "xadapter"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
STATES = {1: "started", 2: "resolve failed", 3: "kernel code mismatch", 4: "waiting for the adapter",
          5: "adapter added", 6: "firmware upload", 7: "re-enumerating", 8: "radio init",
          9: "running", 10: "adapter removed", 11: "failed", 12: "firmware file missing",
          13: "released to the next build"}
OFF_RESET_NODE, OFF_RELEASE = 68, 224  # struct xa_status offsets (xa_plugin.c)
BUTTONS = [(1 << 2, "Menu"), (1 << 3, "View"), (1 << 4, "A"), (1 << 5, "B"), (1 << 6, "X"),
           (1 << 7, "Y"), (1 << 8, "Up"), (1 << 9, "Down"), (1 << 10, "Left"), (1 << 11, "Right"),
           (1 << 12, "LB"), (1 << 13, "RB"), (1 << 14, "LS"), (1 << 15, "RS")]


def pe_va(path, tag):
    """virtual address of a status block's magic in a PE file: the first word-aligned occurrence
    of tag in .data, where every block lives. Other sections are searched only if .data has
    none: the same 4 letters inside a log string in .rdata, e.g. "(XWMT enable 0)", can land
    word-aligned too and must not be taken for the block."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    secs = []
    for k in range(nsec):
        h = pe + 24 + opt + 40 * k
        _, va, rs, ro = struct.unpack_from("<IIII", d, h + 8)
        secs.append((d[h:h + 8].rstrip(b"\0") != b".data", va, rs, ro))
    for _, va, rs, ro in sorted(secs):
        at = d.find(tag, ro, ro + rs)
        while at >= 0 and at % 4:
            at = d.find(tag, at + 1, ro + rs)
        if at >= 0:
            return base + va + at - ro
    raise SystemExit(f"{tag} not found in {path}")


class Xbdm:
    def __init__(self, timeout=10):
        if not os.environ.get("XBOX"):
            raise SystemExit("set XBOX=host[:port] to the console's address")
        host, _, port = os.environ["XBOX"].partition(":")
        import socket
        self.s = socket.create_connection((host, int(port or 730)), timeout=timeout)
        self.f = self.s.makefile("rb")
        self.f.readline()

    def close(self):
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\r\n")
        st = self.f.readline().decode(errors="replace").strip()
        lines = []
        if st.startswith("202"):
            while (l := self.f.readline().decode(errors="replace").strip()) != ".":
                lines.append(l)
        return st, lines

    def mem(self, addr, n):
        st, lines = self.cmd(f"getmem addr=0x{addr:08x} length=0x{n:x}")
        h = "".join(lines)
        return None if not h or "?" in h else bytes.fromhex(h)

    def u32s(self, addr, n):
        b = self.mem(addr, 4 * n)
        return b and struct.unpack(f">{n}I", b)

    def setu32(self, addr, value):
        return self.cmd(f"setmem addr=0x{addr:08x} data={value:08x}")[0]

    def sendfile(self, name, data):
        self.s.sendall(f'sendfile name="{name}" length=0x{len(data):x}\r\n'.encode())
        st = self.f.readline().decode().strip()
        if not st.startswith("204"):
            return st
        self.s.sendall(data)
        return self.f.readline().decode().strip()

    def getfile(self, name):
        self.s.sendall(f'getfile name="{name}"\r\n'.encode())
        st = self.f.readline().decode().strip()
        if not st.startswith("203"):
            raise SystemExit(f"getfile {name}: {st}")
        n = struct.unpack("<I", self.f.read(4))[0]
        return self.f.read(n)

    def modules(self):
        return [l.split()[0].split("=")[1].strip('"').lower() for l in self.cmd("modules")[1]]

    def module_map(self):
        """{name: (base, size)} of the loaded modules"""
        out = {}
        for l in self.cmd("modules")[1]:
            f = dict(kv.split("=", 1) for kv in l.split() if "=" in kv)
            try:
                out[f["name"].strip('"').lower()] = (int(f["base"], 0), int(f["size"], 0))
            except (KeyError, ValueError):
                pass
        return out

    def setbytes(self, addr, data):
        return self.cmd(f"setmem addr=0x{addr:08x} data={data.hex()}")[0]

    def screenshot(self):
        """xbdm's screenshot command: the front buffer as the GPU scans it out, (header, data).
        header: the reply's fields as ints (pitch in bytes, width, height, format = the
        D3DFORMAT word: texture format, endian, tiled and swizzle; framebuffersize; sw, sh =
        the display size); data: framebuffersize raw bytes, still tiled if the format says so.
        Checked on the console (17559 xbdm.xex): 203, one header line, then the bytes."""
        self.s.sendall(b"screenshot\r\n")
        st = self.f.readline().decode(errors="replace").strip()
        if not st.startswith("203"):
            raise SystemExit(f"screenshot: {st}")
        line = self.f.readline().decode(errors="replace")
        hdr = {}
        for kv in line.replace(",", " ").split():
            k, _, v = kv.partition("=")
            try:
                hdr[k] = int(v, 0)
            except ValueError:
                pass
        n = hdr.get("framebuffersize", 0)
        data = self.f.read(n)
        if len(data) != n:
            raise OSError(f"screenshot: {len(data)} of {n} bytes")
        return hdr, data


def xw_slot(name):
    """xw.xex -> 0, xw3.xex -> 3, anything else -> None"""
    n = name.lower()
    if not (n.startswith("xw") and n.endswith(".xex")) or not (n[2:-4].isdigit() or n == "xw.xex"):
        return None
    return int(n[2:-4] or 0)


def scan_block(x, base, size, magic, check=None):
    """address of a 4-byte magic (bytes) at a word boundary in a loaded module's memory, read
    over xbdm page by page; check(address) may reject a candidate. None if it isn't there."""
    for off in range(0, size, 0x1000):
        page = x.mem(base + off, min(0x1000, size - off))
        if not page:
            continue
        at = page.find(magic)
        while at >= 0:
            if at % 4 == 0 and (check is None or check(base + off + at)):
                return base + off + at
            at = page.find(magic, at + 1)
    return None


def scan_blocks(x, base, size, checks):
    """{magic: address} for each magic (an int) in checks whose block is in a loaded module's
    memory, in one pass page by page; checks[magic](address) may reject a candidate."""
    tags = {struct.pack(">I", m): m for m in checks}
    out = {}
    for off in range(0, size, 0x1000):
        if len(out) == len(tags):
            break
        n = min(0x1000, size - off)
        page = x.mem(base + off, n) or x.mem(base + off, n)  # once more: a busy console
        if not page:
            continue
        for tag, m in tags.items():
            at = page.find(tag)
            while m not in out and at >= 0:
                if at % 4 == 0 and (checks[m] is None or checks[m](base + off + at)):
                    out[m] = base + off + at
                at = page.find(tag, at + 1)
    return out


def xw_running(mods):
    """the name (no extension) of the xw build that is running among the loaded modules: the
    one a hot reload loaded last (out/xw_loaded_name) if it is still loaded, else xw.xex"""
    name = name_file("xw_loaded_name", "")
    if name and name + ".xex" in mods:
        return name
    if "xw.xex" in mods:
        return "xw"
    slots = sorted(s for s in (xw_slot(m) for m in mods) if s is not None)
    return f"xw{slots[-1]}" if slots else None


def xw_block(magic):
    """Address of the status block with this magic (an int, as in the C source) in the xw build
    that is running. Normally straight from that build's PE in out/; if the console doesn't
    have the magic there (out/ was rebuilt since the build was loaded), the module's memory
    is searched instead."""
    tag = struct.pack(">I", magic)
    x = Xbdm()
    mods = x.module_map()
    name = xw_running(mods)
    if not name:
        raise SystemExit("no xw build is loaded on the console")
    path = os.path.join(OUT, name + ".exe")
    if os.path.exists(path):
        try:
            addr = pe_va(path, tag)
            w = x.u32s(addr, 1)
            if w and w[0] == magic:
                return addr
        except SystemExit:
            pass
    addr = scan_block(x, *mods[name + ".xex"], tag)
    if addr is None:
        raise SystemExit(f"no {tag.decode(errors='replace')} block in {name}.xex on the console "
                         f"(an older build?)")
    return addr


LAUNCH_INI = "Usb0:" + B + "launch.ini"
BOOT_PATH = "Usb:" + B + "Apps" + B + "xadapter" + B + "xa.xex"


def boot(on, boot_path=None):
    """add (or remove) the plugin as a launch.ini boot plugin, in the first free pluginN slot;
    the original is backed up next to it and in out/ first"""
    BOOT_PATH_ = boot_path or BOOT_PATH
    old = Xbdm().getfile(LAUNCH_INI)
    lines = old.decode("latin-1").split("\n")
    ours = [i for i, l in enumerate(lines)
            if l.strip().lower().startswith("plugin") and l.split("=", 1)[-1].strip().lower() == BOOT_PATH_.lower()]
    if on and ours:
        print(f"already a boot plugin: {lines[ours[0]].strip()}")
        return
    if not on and not ours:
        print("not a boot plugin")
        return
    if on:
        free = [i for i, l in enumerate(lines) if l.strip().lower().startswith("plugin") and "=" in l
                and not l.split("=", 1)[1].strip()]
        if not free:
            raise SystemExit("no free pluginN slot in launch.ini")
        key = lines[free[0]].split("=", 1)[0]
        lines[free[0]] = f"{key}= {BOOT_PATH_}" + ("\r" if lines[free[0]].endswith("\r") else "")
    else:
        for i in ours:
            key = lines[i].split("=", 1)[0]
            lines[i] = f"{key}= " + ("\r" if lines[i].endswith("\r") else "")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    open(os.path.join(OUT, f"launch.ini.{stamp}.bak"), "wb").write(old)
    print("backup", Xbdm().sendfile(f"{LAUNCH_INI}.xa-{stamp}.bak", old))
    new = "\n".join(lines).encode("latin-1")
    print("launch.ini", Xbdm().sendfile(LAUNCH_INI, new))
    if Xbdm().getfile(LAUNCH_INI) != new:
        raise SystemExit("launch.ini read back differs!")
    print("\n".join(l for l in lines if l.strip().lower().startswith("plugin")))


def name_file(which, default):
    """out/built_name (written by build.sh) or out/loaded_name (by load/reload)"""
    try:
        return open(os.path.join(OUT, which)).read().strip() or default
    except OSError:
        return default


def built_name():
    return name_file("built_name", "xa")


def loaded_name():
    return name_file("loaded_name", built_name())


def status_addr(name=None):
    # magic 0x58413336, big-endian in the image of the build that's loaded
    return pe_va(os.path.join(OUT, (name or loaded_name()) + ".exe"), b"XA36")


def wait_for(what, secs, pred):
    for _ in range(int(secs * 4)):
        try:
            v = pred()
            if v:
                return v
        except OSError:
            pass
        time.sleep(0.25)
    raise SystemExit(f"timed out waiting for {what}")


def reload():
    """Replace the running build without a reboot or a replug: build the next slot, have
    the old build release the adapter, load the new one, hand it the adapter."""
    old = loaded_name()
    loaded = Xbdm().modules()
    if old + ".xex" not in loaded:
        old = None
    slot = 1 if not old or old == "xa" else int(old[2:]) + 1
    print(f"building slot {slot} (xa{slot}.xex), replacing {old or 'nothing'}")
    import subprocess
    env = dict(os.environ, XA_SLOT=str(slot))  # build.sh reads OXC (the toolchain) from it
    r = subprocess.run(["bash", "build.sh"], cwd=HERE, env=env, capture_output=True, text=True)
    if r.returncode or built_name() != f"xa{slot}":
        raise SystemExit("build failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    new = built_name()

    node = 0
    if old:
        oa = status_addr(old)
        print(f"{old}: releasing the adapter", Xbdm().setu32(oa + OFF_RELEASE, 1))
        w = wait_for(f"{old} to release", 20, lambda: (lambda w: w if w and w[2] == 13 else None)(
            Xbdm().u32s(oa, 4)))
        node = w[3]
        print(f"{old}: released, adapter node {node:08x}")

    for name, path in [(new + ".xex", os.path.join(OUT, new + ".xex")),
                       ("xa_loader.xex", os.path.join(OUT, "xa_loader.xex"))]:
        print(name, Xbdm().sendfile(FOLDER + B + name, open(path, "rb").read()))
    print("load", Xbdm().cmd(f'magicboot title="{FOLDER}{B}xa_loader.xex" directory="{FOLDER}"')[0])
    na = status_addr(new)
    wait_for(f"{new} to start", 30, lambda: (lambda w: w and w[0] == 0x58413336 and w[2] >= 4)(
        Xbdm().u32s(na, 3)))
    open(os.path.join(OUT, "loaded_name"), "w").write(new + "\n")
    print(f"{new}: waiting for the adapter")
    if node:
        print(f"{new}: taking over node {node:08x}", Xbdm().setu32(na + OFF_RESET_NODE, node))
    else:
        print("no previous build held the adapter: plug it in now")


def read_status(x, addr):
    w = x.u32s(addr, 20)
    if not w or w[0] != 0x58413336:
        return None
    pads = []
    raw = x.mem(addr + 80, 28 * 4)
    for i in range(4):
        conn, wcid, packets, buttons, guide, _, lt, rt, lx, ly, rx, ry = struct.unpack_from(
            ">IIIHBBHHhhhh", raw, 28 * i)
        pads.append(dict(conn=conn, wcid=wcid, packets=packets, buttons=buttons, guide=guide,
                         lt=lt, rt=rt, lx=lx, ly=ly, rx=rx, ry=ry))
    s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v
    return dict(state=w[2], node=w[3], adds=w[4], removes=w[5], matches=w[6], err=s32(w[7]),
                fw=w[8], rx_cmd=w[9], rx_wlan=w[10], rx_err=w[11], log=w[12],
                open=[f"{v:08x}" for v in w[13:17]], heartbeat=w[19], pads=pads)


def pad_line(i, p):
    names = [n for bit, n in BUTTONS if p["buttons"] & bit] + (["Guide"] if p["guide"] else [])
    return (f"pad {i}: {' '.join(names) or '-':<24} LT {p['lt']:4} RT {p['rt']:4} "
            f"L {p['lx']:6},{p['ly']:6} R {p['rx']:6},{p['ry']:6}  ({p['packets']} reports)")


def print_status(s):
    print(f"state {s['state']} ({STATES.get(s['state'], '?')}), node {s['node']:08x}, adds {s['adds']}, "
          f"removes {s['removes']}, match calls {s['matches']}, last error {s['err']}, "
          f"firmware {s['fw']} B, rx cmd/wlan/err {s['rx_cmd']}/{s['rx_wlan']}/{s['rx_err']}, "
          f"open {s['open']}, heartbeat {s['heartbeat']}")
    for i, p in enumerate(s["pads"]):
        if p["conn"]:
            print(pad_line(i, p))


def read_log(x, ring, since):
    hdr = x.u32s(ring, 3)
    if not hdr or hdr[0] != 0x584C4F47:
        return since, ""
    size, written = hdr[1], hdr[2]
    since = max(since, written - size)
    text = b""
    while since < written:
        start = since % size
        n = min(written - since, size - start, 0x1000)
        chunk = x.mem(ring + 12 + start, n)
        if chunk is None:
            break
        text += chunk
        since += n
    return since, text.decode(errors="replace")


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else "status"
    if step == "upload":
        fw = sys.argv[2] if len(sys.argv) > 2 else "/lib/firmware/xone_dongle_02fe.bin"
        name = built_name() + ".xex"
        # keep the adapter's name (xone_dongle_02e6.bin is the original adapter's firmware)
        fw_name = os.path.basename(fw)
        if not (fw_name.startswith("xone_dongle_") and fw_name.endswith(".bin")):
            fw_name = "xone_dongle_02fe.bin"
        files = [(name, os.path.join(OUT, name)), ("xa_loader.xex", os.path.join(OUT, "xa_loader.xex")),
                 (fw_name, fw)]
        for name, path in files:
            data = open(path, "rb").read()
            print(name, len(data), Xbdm().sendfile(FOLDER + B + name, data))
    elif step == "load":
        name = built_name()
        if name + ".xex" in Xbdm().modules():
            raise SystemExit(f"{name}.xex is already loaded (use xa.py reload for a new build)")
        print(Xbdm().cmd(f'magicboot title="{FOLDER}{B}xa_loader.xex" directory="{FOLDER}"')[0])
        open(os.path.join(OUT, "loaded_name"), "w").write(name + "\n")
    elif step == "reload":
        reload()
    elif step == "boot":
        if sys.argv[2:3] not in (["on"], ["off"]):
            raise SystemExit("usage: xa.py boot on|off")
        boot(sys.argv[2] == "on")
    elif step == "aurora":
        a = "Usb0:" + B + "Apps" + B + "Aurora"
        print(Xbdm().cmd(f'magicboot title="{a}{B}Aurora.xex" directory="{a}"')[0])
    elif step == "reclaim":
        m3 = pe_va(os.path.join(OUT, "xa_m3.exe"), b"XAM3")
        w = Xbdm().u32s(m3, 3)
        if not w or w[0] != 0x58414D33 or not w[2]:
            raise SystemExit(f"xa_m3 probe status not found at {m3:08x}: {w}")
        print(f"xa_m3 probe holds node {w[2]:08x}; asking xa to re-enumerate it")
        print(Xbdm().setu32(status_addr() + 68, w[2]))
    elif step == "pair":
        print(Xbdm().setu32(status_addr() + 72, 1))
    elif step == "unload":
        print(Xbdm().setu32(status_addr() + 220, 1))  # tear down and self-unload (no reboot)
    elif step == "hold":
        print(Xbdm().setu32(status_addr() + 192, int(sys.argv[2])))
    elif step == "step":
        print(Xbdm().setu32(status_addr() + 196, 1))
    elif step == "reg":
        base = status_addr()
        addr = int(sys.argv[2], 0)
        x = Xbdm()
        x.setu32(base + 208, addr)
        if len(sys.argv) > 3:
            x.setu32(base + 212, int(sys.argv[3], 0))
        x.setu32(base + 204, 2 if len(sys.argv) > 3 else 1)
        for _ in range(100):  # the thread services it between its other calls
            time.sleep(0.1)
            w = Xbdm().u32s(base + 204, 4)
            if w and w[0] == 0:
                st = w[3] - (1 << 32) if w[3] & 0x80000000 else w[3]
                print(f"{addr:08x} = {w[2]:08x} (status {st})" if len(sys.argv) <= 3
                      else f"{addr:08x} <- {int(sys.argv[3], 0):08x} (status {st})")
                break
        else:
            print("no answer from the driver thread")
    elif step == "led":
        print(Xbdm().setu32(status_addr() + 200, 0 if int(sys.argv[2]) else 1))
    elif step in ("watch", "status"):
        addr = status_addr()
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else (0 if step == "status" else 120)
        since, last_pads, until = 0, None, time.time() + secs
        while True:
            try:
                x = Xbdm()
                s = read_status(x, addr)
                if s is None:
                    print(f"xa status block not found at {addr:08x} (plugin loaded?)")
                else:
                    since, text = read_log(x, s["log"], since)
                    if text:
                        print(text, end="", flush=True)
                    pads = [pad_line(i, p) for i, p in enumerate(s["pads"]) if p["conn"]]
                    if step == "status":
                        print_status(s)
                    elif pads != last_pads:
                        print("\n".join(pads) or "(no pads)", flush=True)
                        last_pads = pads
            except OSError as e:
                print(f"xbdm: {e}", flush=True)
            if time.time() >= until:
                break
            time.sleep(0.3)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
