#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the bytes of xw.xex go: per source file and per symbol, split into code (.text),
constants (.rdata) and data (.data), from xbox360/out/xw.exe (the PE that becomes xw.xex).

  tools/size_report.py [--exe PATH] [--top N]       the report
  tools/size_report.py --save BASELINE.json         ... and store it as a baseline
  tools/size_report.py --diff BASELINE.json         what changed against a stored baseline
  tools/size_report.py --map-only                   only make (or check) the link map
  tools/size_report.py --snapshot DIR               copy xw.exe and its map to DIR (for
                                                    tools/fn_diff.py: a "before" build)

The PE carries no symbols, so the names come from lld-link's map (/lldmap). build.sh does
not write one, so this tool makes it: it re-runs xbox360/build.sh in the toolchain container
(xw-toolchain, as toolchain/build.sh) on a throw-away copy of the tree, with clang wrapped so
that the link of xw.exe also writes out/xw.lldmap, and then checks that the rebuilt xw.exe is
byte-identical to out/xw.exe (all but the link time stamp). If it is not, out/xw.exe was
not built from the tree as it is now: rebuild first (bash toolchain/build.sh). The map is
kept next to the exe (xw.lldmap, with xw.lldmap.sum naming the exe it belongs to) and reused
while the exe is unchanged.

Sizes are spans in the image: a symbol runs to the next symbol of its object file, so it
includes alignment padding. ".L" names are the compiler's anonymous labels: string literals
are summed as "(strings)" per file, other anonymous spans as "(anonymous)". What the linker
adds itself (import thunks after the code, import tables after the constants) is listed
under "(linker)".
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "xbox360", "out")
EXE = os.path.join(OUT, "xw.exe")
IMAGE = "xw-toolchain"  # toolchain/build.sh
LIMIT = 0x100000  # the image must stay under this (CLAUDE.md)
KINDS = {".text": "code", ".rdata": "const", ".data": "data", ".bss": "data"}
LINKER = "(linker)"
# a link-map line: address, size, alignment, then the output section, an input section
# ("object:(section)") or a symbol, by its column
MAP_LINE = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]{8}) +(\d+) (.*)$")


# ---------------------------------------------------------------------------------- PE file

def pe_read(path):
    """the parts of a PE this tool needs: image base and size, time stamp offset, sections
    [(name, rva, vsize, raw_off, raw_size)], and the import directory (rva, size)"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE file")
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", d, opt + 28)[0]
    size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
    imp_rva, imp_size = struct.unpack_from("<II", d, opt + 96 + 8)
    secs = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        name = d[s:s + 8].rstrip(b"\0").decode()
        vsize, rva, raw_size, raw_off = struct.unpack_from("<IIII", d, s + 8)
        secs.append((name, rva, vsize, raw_off, raw_size))
    return dict(path=path, data=d, base=base, size=size_of_image, stamp_off=pe + 8,
                sections=secs, imports=(imp_rva, imp_size))


def pe_bytes(pe, rva, n):
    """n bytes of the image at rva (zeros past a section's raw data)"""
    for name, srva, vsize, raw_off, raw_size in pe["sections"]:
        if srva <= rva < srva + max(vsize, raw_size):
            off = rva - srva
            b = pe["data"][raw_off + off:raw_off + min(off + n, raw_size)]
            return b + bytes(n - len(b))
    return bytes(n)


def pe_digest(path):
    """sha256 of the file with the link time stamp zeroed: two links of the same objects
    have the same digest"""
    pe = pe_read(path)
    d = bytearray(pe["data"])
    d[pe["stamp_off"]:pe["stamp_off"] + 4] = bytes(4)
    return hashlib.sha256(d).hexdigest()


def pe_split(pe):
    """{"code": bytes, "const": bytes, "data": bytes} by section, and the image size"""
    split = {"code": 0, "const": 0, "data": 0}
    for name, rva, vsize, raw_off, raw_size in pe["sections"]:
        split[KINDS.get(name, "data")] += vsize
    return split


def pe_imports(pe):
    """{IAT entry rva: "module#ordinal" or "module!name"} from the import directory"""
    rva, size = pe["imports"]
    out = {}
    if not rva:
        return out
    u32 = lambda a: struct.unpack("<I", pe_bytes(pe, a, 4))[0]
    for i in range(0, 0x1000, 20):
        oft, _, _, name_rva, ft = struct.unpack("<5I", pe_bytes(pe, rva + i, 20))
        if not name_rva:
            break
        mod = pe_bytes(pe, name_rva, 64).split(b"\0")[0].decode(errors="replace")
        mod = mod.split("@")[0].rsplit(".", 1)[0]  # "xboxkrnl.exe@<version>" -> xboxkrnl
        lookup = oft or ft
        for j in range(0, 0x4000, 4):
            v = u32(lookup + j)
            if not v:
                break
            if v & 0x80000000:
                what = f"{mod}#{v & 0xFFFF}"
            else:
                what = mod + "!" + pe_bytes(pe, v + 2, 64).split(b"\0")[0].decode(errors="replace")
            out[ft + j] = what
    return out


# ------------------------------------------------------------------------------- link map

def obj_name(path):
    """/tmp/xw_plugin-96928e.o -> xw_plugin; libc_a-memcpy.o -> libc:memcpy;
    udivdi3.c.obj -> rt:udivdi3 (clang's temporary objects carry a random suffix)"""
    b = os.path.basename(path)
    m = re.match(r"libc_a-(.+)\.o$", b)
    if m:
        return "libc:" + m.group(1)
    if b.endswith(".c.obj"):
        return "rt:" + b[:-6]
    return re.sub(r"-[0-9a-f]{6}\.o$", "", b)


def source_of(stem, root=ROOT):
    """xw_plugin -> xbox360/xw_plugin.c (the file it was compiled from), else the stem"""
    if ":" in stem:
        return stem
    for d in ("xbox360", "core", "third_party/micro-ecc"):
        for ext in (".c", ".s", ".cpp"):
            p = os.path.join(d, stem + ext)
            if os.path.exists(os.path.join(root, p)):
                return p
    return stem


def parse_map(path, root=ROOT):
    """lld-link /lldmap -> dict(sections=[(name, rva, size)], contribs=[dict(sec, rva, size,
    file, syms=[(rva, [names])])]); symbols at one address are grouped, in map order"""
    sections, contribs = [], []
    with open(path) as f:
        for line in f:
            m = MAP_LINE.match(line.rstrip("\n"))
            if not m:
                continue
            rva, size, rest = int(m.group(1), 16), int(m.group(2), 16), m.group(4)
            indent = len(rest) - len(rest.lstrip())  # the column: 0, 8 or 16
            if indent < 4:  # output section
                sections.append((rest.strip(), rva, size))
            elif indent < 12:  # input section "object:(section)"
                obj, _, sec = rest.strip().rpartition(":(")
                contribs.append(dict(sec=sections[-1][0], rva=rva, size=size,
                                     file=source_of(obj_name(obj), root), syms=[]))
            else:  # symbol
                name, syms = rest.strip(), contribs[-1]["syms"]
                if syms and syms[-1][0] == rva:
                    if name not in syms[-1][1]:
                        syms[-1][1].append(name)
                else:
                    syms.append((rva, [name]))
    if not sections:
        raise SystemExit(f"{path}: not an lld-link /lldmap file")
    return dict(sections=sections, contribs=contribs)


def canonical(names):
    """the name a group of symbols at one address goes by: the first that is not a .L label"""
    for n in names:
        if not n.startswith(".L"):
            return n
    return None


def pseudo(names):
    return "(strings)" if any(n.startswith(".L.str") for n in names) else "(anonymous)"


def symbols(mp):
    """[dict(name, file, kind, sec, rva, size)]: every span of the image, the linker's
    additions included; anonymous spans carry a pseudo name"""
    out = []
    for c in mp["contribs"]:
        if not c["size"]:
            continue
        syms = sorted(c["syms"])
        end = c["rva"] + c["size"]
        if not syms or syms[0][0] > c["rva"]:
            syms.insert(0, (c["rva"], [".L(start)"]))
        for i, (rva, names) in enumerate(syms):
            nxt = syms[i + 1][0] if i + 1 < len(syms) else end
            if rva >= end or nxt <= rva:
                continue
            name = canonical(names)
            out.append(dict(name=name or pseudo(names), file=c["file"], kind=KINDS.get(c["sec"],
                            "data"), sec=c["sec"], rva=rva, size=min(nxt, end) - rva,
                            anon=name is None))
    # what the linker added at the end of a section (import thunks, import tables)
    for name, rva, size in mp["sections"]:
        used = [c["rva"] + c["size"] for c in mp["contribs"] if c["sec"] == name and c["size"]]
        tail = max(used, default=rva)
        if tail < rva + size:
            out.append(dict(name=f"(linker {name})", file=LINKER, kind=KINDS.get(name, "data"),
                            sec=name, rva=tail, size=rva + size - tail, anon=True))
    return out


def map_matches(mp, pe):
    """the map's output sections have the PE's sizes"""
    have = {n: s for n, _, s in mp["sections"] if s}
    want = {n: v for n, _, v, _, _ in pe["sections"]}
    return all(want.get(n) == s for n, s in have.items())


# ------------------------------------------------------------------ making the map (Docker)

# Runs in the toolchain container: the tree (/src, read-only) is copied to /tmp/w without
# its build output, clang is wrapped so that the xw.exe link also writes /work/xw.lldmap,
# build.sh runs there, and the rebuilt xw.exe is copied out for the comparison.
SHIM = r"""
set -e
mkdir -p /tmp/w /tmp/shim/bin
tar -C /src --exclude=./xbox360/out --exclude=./research --exclude=./.git -cf - . | tar -C /tmp/w -xf -
for f in "$OXC"/bin/*; do ln -s "$f" /tmp/shim/bin/; done
rm /tmp/shim/bin/clang
cat > /tmp/shim/bin/clang <<EOF
#!/bin/bash
for a in "\$@"; do
    [ "\$a" = out/xw.exe ] && exec "$OXC/bin/clang" "\$@" -Wl,/lldmap:/work/xw.lldmap
done
exec "$OXC/bin/clang" "\$@"
EOF
chmod +x /tmp/shim/bin/clang
cd /tmp/w/xbox360
OXC=/tmp/shim bash build.sh > /work/build.log 2>&1 || { tail -30 /work/build.log; exit 1; }
cp out/xw.exe /work/xw.exe
"""


def map_path(exe):
    return os.path.splitext(exe)[0] + ".lldmap"


def map_is_current(exe):
    """the map next to exe was made for this exe: its .sum names the exe's digest, or (no
    .sum) the link wrote both, so they are seconds apart (build.sh with /lldmap)"""
    mp, s = map_path(exe), map_path(exe) + ".sum"
    if not os.path.exists(mp) or not os.path.exists(exe):
        return False
    if os.path.exists(s) and open(s).read().split()[0] == pe_digest(exe):
        return True
    return abs(os.path.getmtime(mp) - os.path.getmtime(exe)) < 10


def docker_run(args, mounts, env=()):
    cmd = ["docker", "run", "--rm"]
    for e in env:
        cmd += ["-e", e]
    for host, guest in mounts:
        cmd += ["-v", f"{host}:{guest}"]
    try:
        return subprocess.run(cmd + [IMAGE] + args, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit("docker is not installed (the toolchain runs in Docker: toolchain/build.sh)")


def make_map(exe=EXE, root=ROOT, quiet=False):
    """makes (or reuses) the link map of exe, which must be the tree's own out/xw.exe:
    returns its path"""
    if map_is_current(exe):
        return map_path(exe)
    if os.path.abspath(exe) != os.path.abspath(os.path.join(root, "xbox360", "out", "xw.exe")):
        raise SystemExit(f"no current link map for {exe} ({map_path(exe)} with its .sum): make "
                         "one with tools/size_report.py --snapshot DIR when the build is made")
    if not os.path.exists(exe):
        raise SystemExit(f"{exe}: no build (bash toolchain/build.sh)")
    if not quiet:
        print("making the link map: rebuilding a copy of the tree in the toolchain container "
              "(about a minute)...", file=sys.stderr, flush=True)
    work = os.path.join(os.path.dirname(exe), ".mapwork")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    try:
        r = docker_run(["bash", "-c", SHIM], [(root, "/src:ro"), (work, "/work")],
                       env=("XW_STAGED", "XA_HOOKS"))
        if r.returncode:
            raise SystemExit(f"the map build failed:\n{r.stdout}{r.stderr}")
        if pe_digest(os.path.join(work, "xw.exe")) != pe_digest(exe):
            raise SystemExit(f"{exe} is not the build of this tree as it is now (the rebuilt "
                             "xw.exe differs): run bash toolchain/build.sh first")
        shutil.move(os.path.join(work, "xw.lldmap"), map_path(exe))
        with open(map_path(exe) + ".sum", "w") as f:
            f.write(pe_digest(exe) + "  " + os.path.basename(exe) + "\n")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return map_path(exe)


def load(exe, mapfile=None):
    """(pe, map, symbols) of a build; the map is made if needed"""
    pe = pe_read(exe)
    mp = parse_map(mapfile or make_map(exe))
    if not map_matches(mp, pe):
        raise SystemExit(f"the link map does not match {exe} (section sizes differ)")
    return pe, mp, symbols(mp)


# -------------------------------------------------------------------------------- report

def summarise(pe, syms):
    """the report as plain data (what --save stores)"""
    files, by_key, seen = {}, {}, {}
    for s in syms:
        f = files.setdefault(s["file"], {"code": 0, "const": 0, "data": 0})
        f[s["kind"]] += s["size"]
        seen.setdefault(s["name"], set()).add(s["file"])
    for s in syms:
        # one key per symbol: its name, or name@file for an anonymous span or a static name
        # that more than one file uses (a moved function keeps its key)
        key = s["name"] if not s["anon"] and len(seen[s["name"]]) == 1 else f"{s['name']}@{s['file']}"
        e = by_key.setdefault(key, dict(name=s["name"], file=s["file"], kind=s["kind"], size=0))
        e["size"] += s["size"]
    return dict(image=pe["size"], sections=pe_split(pe), files=files, symbols=by_key)


def kb(n):
    return f"{n / 1024:.1f}K"


def print_report(r, top):
    sp = r["sections"]
    print(f"image {r['image']:#x} ({r['image']} bytes, {kb(r['image'])}; limit {LIMIT:#x}, "
          f"{LIMIT - r['image']} bytes left)")
    print(f"code {sp['code']} ({kb(sp['code'])}), constants {sp['const']} ({kb(sp['const'])}), "
          f"data {sp['data']} ({kb(sp['data'])})")
    print()
    print(f"{'file':<34} {'code':>8} {'const':>8} {'data':>8} {'total':>8}")
    for name, f in sorted(r["files"].items(), key=lambda kv: -sum(kv[1].values())):
        print(f"{name:<34} {f['code']:>8} {f['const']:>8} {f['data']:>8} {sum(f.values()):>8}")
    print()
    print(f"top {top} symbols:")
    print(f"{'bytes':>8} {'kind':<5} {'symbol':<36} file")
    for e in sorted(r["symbols"].values(), key=lambda e: -e["size"])[:top]:
        print(f"{e['size']:>8} {e['kind']:<5} {e['name']:<36} {e['file']}")


def print_diff(old, new, top):
    def line(label, a, b):
        return f"{label:<36} {a:>8} -> {b:>8}  {b - a:+d}"
    print(line("image", old["image"], new["image"]))
    for k in ("code", "const", "data"):
        print(line(k, old["sections"][k], new["sections"][k]))
    print()
    rows = []
    for name in sorted(set(old["files"]) | set(new["files"])):
        a = sum(old["files"].get(name, {}).values())
        b = sum(new["files"].get(name, {}).values())
        if a != b:
            rows.append((b - a, line(name, a, b)))
    print("files changed:" if rows else "files: no change")
    for _, t in sorted(rows, key=lambda r: -abs(r[0])):
        print("  " + t)
    print()
    rows = []
    for key in set(old["symbols"]) | set(new["symbols"]):
        a, b = old["symbols"].get(key), new["symbols"].get(key)
        sa, sb = a["size"] if a else 0, b["size"] if b else 0
        if sa != sb:
            what = "added" if not a else "removed" if not b else (b or a)["kind"]
            rows.append((sb - sa, f"{key:<44} {sa:>8} -> {sb:>8}  {sb - sa:+d}  {what}"))
    print(f"symbols changed: {len(rows)}" + (f" (largest {top})" if len(rows) > top else ""))
    for _, t in sorted(rows, key=lambda r: -abs(r[0]))[:top]:
        print("  " + t)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", default=EXE, help="the PE (default xbox360/out/xw.exe)")
    ap.add_argument("--map", help="its lld-link /lldmap (default: next to it, made if needed)")
    ap.add_argument("--top", type=int, default=25, help="symbols listed (default 25)")
    ap.add_argument("--save", metavar="JSON", help="store the report as a baseline")
    ap.add_argument("--diff", metavar="JSON", help="compare with a stored baseline")
    ap.add_argument("--map-only", action="store_true", help="only make the link map")
    ap.add_argument("--snapshot", metavar="DIR", help="copy the exe and its map to DIR")
    a = ap.parse_args()
    if a.map_only or a.snapshot:
        m = a.map or make_map(a.exe)
        if a.snapshot:
            os.makedirs(a.snapshot, exist_ok=True)
            dst = os.path.join(a.snapshot, os.path.basename(a.exe))
            shutil.copy2(a.exe, dst)
            shutil.copy2(m, map_path(dst))
            with open(map_path(dst) + ".sum", "w") as f:
                f.write(pe_digest(dst) + "  " + os.path.basename(dst) + "\n")
            print(f"{dst} and {map_path(dst)}")
        else:
            print(m)
        return
    pe, _, syms = load(a.exe, a.map)
    r = summarise(pe, syms)
    if a.diff:
        with open(a.diff) as f:
            print_diff(json.load(f), r, a.top)
    else:
        print_report(r, a.top)
    if a.save:
        with open(a.save, "w") as f:
            json.dump(r, f, indent=1, sort_keys=True)
        print(f"\nsaved {a.save}")


if __name__ == "__main__":
    main()
