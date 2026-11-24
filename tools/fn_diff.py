#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Function-by-function comparison of two xw.exe builds: the proof that a refactor (a file
split, moved code, a fenced block) did not change what the code does.

  tools/fn_diff.py OLD.exe NEW.exe [--lines N] [--all] [--objdump PATH]

Each exe needs its link map next to it (OLD.lldmap, made by tools/size_report.py):

  python3 tools/size_report.py --snapshot /tmp/before     # before the change
  ...change, bash toolchain/build.sh...
  python3 tools/fn_diff.py /tmp/before/xw.exe xbox360/out/xw.exe

Both are disassembled (llvm-objdump in the toolchain container, or --objdump for a host
one), and every instruction is normalised before the comparison, so that code which only
moved compares equal:
  - branch and call targets become the target's name (a local target: its offset in the
    function; an import thunk: the import, e.g. imp:xboxkrnl#153);
  - an address built from lis + a low half (addi, a load or store, ori) becomes the name of
    what it points at plus an offset; a string literal becomes its text, any other anonymous
    constant its normalised bytes, an import table entry its import;
  - trailing alignment padding is dropped.
Named constants and initialised data are compared the same way (their words that point
into the image become names). The relocations are not in the PE, so addresses are tracked
through each function's registers (a data flow over its basic blocks); an address the
tracking loses shows as a raw offset, which can make a function look changed when it is not
(its diff shows it).

Functions match by name (name@file when a static name is used in more than one file).
Output: counts of identical / changed / added / removed functions and objects, the ones
that moved file, and a short diff for each changed function. Exit status 1 if anything
changed, was added or was removed.
"""
import argparse
import bisect
import difflib
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import size_report as sr  # noqa: E402

INSN = re.compile(r"^\s*([0-9a-f]{8}):\s+((?:[0-9a-f]{2} ){4})\s*(.*)$")
TARGET = re.compile(r"0x[0-9a-f]+ <[^>]*>")
VOLATILE = [0] + list(range(3, 13))  # clobbered by a call
# X-form (opcode 31) extended opcodes that write no GPR / write rA (logical, shifts)
X_NONE = {0, 32, 4, 68, 151, 215, 407, 149, 662, 918, 150, 214, 54, 86, 246, 278, 1014, 982,
          598, 854, 467, 144, 146, 178, 210, 242, 306, 370, 566, 274, 402, 434, 695, 759, 727,
          663, 983}
X_UPDATE_STORE = {183, 247, 439, 181}
X_RA = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 954, 922, 986, 27, 539,
        794, 58}
X_LOAD_UPDATE = {55, 119, 311, 375, 53, 373}
D_LOADS = set(range(32, 36)) | set(range(40, 44))  # lwz lwzu lbz lbzu lhz lhzu lha lhau
D_STORES = {36, 37, 38, 39, 44, 45}
D_FORMS = D_LOADS | D_STORES | {14, 46, 47, 58, 62}  # + addi lmw stmw ld* std*
NOPS = {0, 0x60000000}


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def writes(w):
    """the GPRs an instruction writes (as far as address tracking needs to know)"""
    op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in X_NONE:
            return ()
        if xo in X_UPDATE_STORE:
            return (ra,)
        if xo in X_RA or (w >> 2) & 0x1FF == 413:  # sradi
            return (ra,)
        if xo in X_LOAD_UPDATE:
            return (rd, ra)
        return (rd,)
    if op in (7, 8, 12, 13, 14, 15) or op in D_LOADS or op == 58:
        upd = op in (33, 35, 41, 43) or (op == 58 and w & 3 == 1)
        return (rd, ra) if upd else (rd,)
    if op == 46:  # lmw
        return tuple(range(rd, 32))
    if op in (37, 39, 45) or (op == 62 and w & 3 == 1):
        return (ra,)
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return (ra,)
    return ()


def is_call(w):
    op = w >> 26
    if op in (16, 18):
        return bool(w & 1)
    return op == 19 and w & 1 and ((w >> 1) & 0x3FF) in (16, 528)  # bclrl, bcctrl


class Build:
    """one exe: its symbols, functions, and a resolver from addresses to stable names"""

    def __init__(self, exe, mapfile, objdump):
        self.exe = exe
        self.pe, self.mp, syms = sr.load(exe, mapfile)
        self.base = self.pe["base"]
        self.iat = {self.base + r: n for r, n in sr.pe_imports(self.pe).items()}
        # every map symbol group in address order, for "what is at this address"
        self.groups = []
        for c in self.mp["contribs"]:
            for rva, names in c["syms"]:
                self.groups.append((self.base + rva, names, c))
        self.groups.sort(key=lambda g: g[0])
        self.starts = [g[0] for g in self.groups]
        self.secs = {n: (self.base + r, self.base + r + s) for n, r, s in self.mp["sections"]}
        self.text = self.secs[".text"]
        self.insns = disassemble(exe, objdump)
        self.funcs = self._functions(syms)
        self.fstarts = sorted(self.funcs.values(), key=lambda f: f["start"])
        self._fkeys = [f["start"] for f in self.fstarts]

    def _functions(self, syms):
        seen = {}
        code = [s for s in syms if s["sec"] == ".text" and not s["anon"]]
        for s in code:
            seen.setdefault(s["name"], set()).add(s["file"])
        out = {}
        for s in code:
            key = s["name"] if len(seen[s["name"]]) == 1 else f"{s['name']}@{s['file']}"
            start = self.base + s["rva"]
            end = start + s["size"]
            while end > start and self.word(end - 4) in NOPS:
                end -= 4
            out[key] = dict(key=key, name=s["name"], file=s["file"], start=start, end=end)
        # the import thunks the linker put after the code: lis r11 / lwz r11 / mtctr / bctr
        for s in syms:
            if s["sec"] == ".text" and s["file"] == sr.LINKER:
                a = self.base + s["rva"]
                for t in range(a, a + s["size"] - 15, 16):
                    hi, lo = self.word(t), self.word(t + 4)
                    slot = ((hi & 0xFFFF) << 16) + sext(lo & 0xFFFF, 16) & 0xFFFFFFFF
                    name = "imp:" + self.iat.get(slot, f"?{slot:08x}")
                    out[name] = dict(key=name, name=name, file=sr.LINKER, start=t, end=t + 16)
        return out

    def word(self, va):
        return struct.unpack(">I", sr.pe_bytes(self.pe, va - self.base, 4))[0]

    def in_image(self, va):
        return self.base <= va < self.base + self.pe["size"]

    def func_at(self, va):
        i = bisect.bisect_right(self._fkeys, va) - 1
        if i >= 0:
            f = self.fstarts[i]
            if va < f["end"] or (va == f["start"]):
                return f
        return None

    def code_ref(self, va, cur=None):
        if cur and cur["start"] <= va < cur["end"]:
            return f"<+{va - cur['start']:#x}>"
        f = self.func_at(va)
        if f:
            return f["key"] + (f"+{va - f['start']:#x}" if va != f["start"] else "")
        return f"?{va - self.text[0]:#x}"

    def data_ref(self, va, depth=0):
        """a name for an address that does not change when code or data merely moves"""
        if self.text[0] <= va < self.text[1]:
            return "&" + self.code_ref(va)
        if va in self.iat:
            return "iat:" + self.iat[va]
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0:
            return f"?{va:08x}"
        start, names, c = self.groups[i]
        cend = self.base + c["rva"] + c["size"]
        if va >= cend:
            return f"?{c['sec']}"
        name = sr.canonical(names)
        if name:
            return name + (f"+{va - start:#x}" if va != start else "")
        # anonymous: by content, up to the next label
        nxt = self.starts[i + 1] if i + 1 < len(self.starts) else cend
        n = max(0, min(nxt, cend, va + 256) - va)
        b = sr.pe_bytes(self.pe, va - self.base, n)
        z = b.find(b"\0")
        if c["sec"] == ".rdata" and z >= 0 and all(32 <= ch < 127 or ch in (9, 10, 13) for ch in b[:z]):
            s = b[:z].decode()
            return 'str"' + (s if len(s) <= 40 else s[:37] + "...") + '"' + \
                (f"#{hashlib.sha1(b[:z]).hexdigest()[:6]}" if len(s) > 40 else "")
        if depth:  # an anonymous object pointing at another: name the second by size only
            return f"anon{c['sec']}[{n}]"
        return f"anon{c['sec']}[{n}:{self.content_hash(va, n)}]"

    def content(self, va, n, depth=0):
        """normalised bytes: aligned words that point into the image become names"""
        b = sr.pe_bytes(self.pe, va - self.base, n)
        parts = []
        for o in range(0, n - n % 4, 4):
            w = struct.unpack_from(">I", b, o)[0]
            parts.append(self.data_ref(w, depth + 1) if w and self.in_image(w) and w != va + o
                         else f"{w:08x}")
        return parts + [b[n - n % 4:].hex()]

    def content_hash(self, va, n):
        return hashlib.sha1(" ".join(self.content(va, n, 1)).encode()).hexdigest()[:8]

    def step(self, regs, va, w, text, f):
        """one instruction: (normalised text, register state after it). regs maps a register
        to ("hi", value) after lis, or ("full", address) after lis + a low half"""
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        norm = TARGET.sub(lambda m: self.code_ref(int(m.group(0).split()[0], 16), f), text)
        new = None
        hi = (w & 0xFFFF) << 16
        if op == 15 and ra == 0 and self.base - 0x10000 <= hi < self.base + self.pe["size"] + 0x10000:
            norm = f"lis {rd}, @ha"  # the low half that follows names the address
            new = (rd, ("hi", hi))
        elif op in D_FORMS and ra and ra in regs:
            kind, val = regs[ra]
            disp = sext(w & (0xFFFC if op in (58, 62) else 0xFFFF), 16)
            addr = (val + disp) & 0xFFFFFFFF
            if self.in_image(addr) or kind == "hi":
                ref = self.data_ref(addr)
                norm = re.sub(r"-?\d+\((\d+)\)$", lambda m: f"{ref}({m.group(1)})", text)
                if op == 14:
                    norm = re.sub(r", -?\d+$", lambda m: f", {ref}", text)
                    new = (rd, ("full", addr))
        elif op == 24 and rd in regs and regs[rd][0] == "hi":  # ori rA, rS, lo
            addr = regs[rd][1] | (w & 0xFFFF)
            norm = re.sub(r", \d+$", lambda m: ", " + self.data_ref(addr), text)
            new = (ra, ("full", addr))
        out = dict(regs)
        for r in writes(w):
            out.pop(r, None)
        if is_call(w):
            for r in VOLATILE:
                out.pop(r, None)
        if new:
            out[new[0]] = new[1]
        return norm, out

    def blocks(self, f):
        """the function's basic blocks: ([start va], {start: [successor starts]})"""
        lo, hi = f["start"], f["end"]
        leaders, edges, indirect = {lo}, {}, []
        for va in range(lo, hi, 4):
            w = self.insns.get(va, (self.word(va), ""))[0]
            op = w >> 26
            if op in (16, 18) or (op == 19 and ((w >> 1) & 0x3FF) in (16, 528)):
                leaders.add(va + 4)
                if op in (16, 18) and not w & 3:  # relative, not a call
                    t = va + (sext(w & 0xFFFC, 16) if op == 16 else sext(w & 0x3FFFFFC, 26))
                    if lo <= t < hi:
                        leaders.add(t)
        starts = sorted(x for x in leaders if lo <= x < hi)
        for k, b in enumerate(starts):
            end = starts[k + 1] if k + 1 < len(starts) else hi
            last = end - 4
            w = self.insns.get(last, (self.word(last), ""))[0]
            op, bo = w >> 26, (w >> 21) & 31
            always = bo & 0x14 == 0x14
            succ = []
            if op == 18 and not w & 1:  # b: its target (outside: a tail call)
                t = (last + sext(w & 0x3FFFFFC, 26)) & 0xFFFFFFFF
                succ = [t] if lo <= t < hi else []
            elif op == 16 and not w & 1:
                t = (last + sext(w & 0xFFFC, 16)) & 0xFFFFFFFF
                succ = ([t] if lo <= t < hi else []) + ([] if always else [end])
            elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and not w & 1:
                if ((w >> 1) & 0x3FF) == 528 and always:
                    indirect.append(b)  # bctr: a jump table (or a tail call)
                succ = [] if always else [end]
            else:
                succ = [end]
            edges[b] = [s for s in succ if s < hi]
        # a block nothing branches to follows a jump table's bctr
        reached = {s for v in edges.values() for s in v} | {lo}
        for b in starts:
            if b not in reached:
                for src in indirect:
                    edges[src].append(b)
        return starts, edges

    def normalise(self, f):
        """[(token, text)] for a function: token is compared, text is shown. Addresses are
        tracked by a forward data flow over the basic blocks: a register keeps its value at
        a join only if every path into it agrees"""
        starts, edges = self.blocks(f)
        ends = {b: (starts[k + 1] if k + 1 < len(starts) else f["end"]) for k, b in enumerate(starts)}
        preds = {b: [] for b in starts}
        for b, ss in edges.items():
            for s in ss:
                if s in preds:
                    preds[s].append(b)
        state_out = {}

        def state_in(b):
            """None while no path into b has been followed yet"""
            if b == f["start"] or not preds[b]:
                return {}  # the entry, or a block nothing reaches: nothing known
            known = [state_out[p] for p in preds[b] if p in state_out]
            if not known:
                return None
            first = known[0]
            return {r: v for r, v in first.items() if all(o.get(r) == v for o in known[1:])}

        work = list(starts)
        while work:
            b = work.pop(0)
            regs = state_in(b)
            if regs is None:
                continue  # comes back when a block before it is done
            for va in range(b, ends[b], 4):
                w, text = self.insns.get(va, (self.word(va), "?"))
                regs = self.step(regs, va, w, text, f)[1]
            if state_out.get(b) != regs:
                state_out[b] = regs
                work += [s for s in edges[b] if s not in work]
        out = []
        for b in starts:
            regs = state_in(b) or {}
            for va in range(b, ends[b], 4):
                w, text = self.insns.get(va, (self.word(va), "?"))
                norm, regs = self.step(regs, va, w, text, f)
                out.append((norm, norm))
        return out

    def objects(self):
        """named constants and initialised data: {key: (file, size, normalised content)}"""
        out, seen = {}, {}
        named = []
        for c in self.mp["contribs"]:
            if c["sec"] not in (".rdata", ".data") or not c["size"]:
                continue
            syms = sorted(c["syms"])
            for i, (rva, names) in enumerate(syms):
                name = sr.canonical(names)
                if not name:
                    continue
                nxt = syms[i + 1][0] if i + 1 < len(syms) else c["rva"] + c["size"]
                named.append((name, c["file"], self.base + rva, nxt - rva))
                seen.setdefault(name, set()).add(c["file"])
        for name, file, va, n in named:
            key = name if len(seen[name]) == 1 else f"{name}@{file}"
            parts = self.content(va, n)
            while parts and parts[-1] in ("00000000", ""):  # zero tail: alignment, or a zeroed array
                parts.pop()
            out[key] = (file, n, parts)
        return out


def disassemble(exe, objdump=None):
    """{va: (word, "mnemonic operands")} of the exe's code"""
    if objdump:
        import subprocess
        txt = subprocess.run([objdump, "-d", exe], capture_output=True, text=True, check=True).stdout
    else:
        d = os.path.dirname(os.path.abspath(exe))
        r = sr.docker_run(["bash", "-c", f'"$OXC/bin/llvm-objdump" -d "/x/{os.path.basename(exe)}"'],
                          [(d, "/x:ro")])
        if r.returncode:
            raise SystemExit(f"llvm-objdump failed: {r.stderr}")
        txt = r.stdout
    out = {}
    for line in txt.splitlines():
        m = INSN.match(line)
        if m:
            w = int(m.group(2).replace(" ", ""), 16)
            out[int(m.group(1), 16)] = (w, " ".join(m.group(3).split()))
    if not out:
        raise SystemExit(f"{exe}: no instructions disassembled")
    return out


def find_map(exe):
    if sr.map_is_current(exe):
        return sr.map_path(exe)
    return None  # sr.load makes one for the tree's own out/xw.exe, or explains


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--lines", type=int, default=24, help="diff lines per changed function")
    ap.add_argument("--all", action="store_true", help="list identical functions too")
    ap.add_argument("--objdump", help="a host llvm-objdump with PowerPC (default: the container's)")
    a = ap.parse_args()
    old = Build(a.old, find_map(a.old), a.objdump)
    new = Build(a.new, find_map(a.new), a.objdump)

    names = sorted(set(old.funcs) | set(new.funcs))
    same, changed, added, removed, moved, shifted = [], [], [], [], [], 0
    for k in names:
        fo, fn = old.funcs.get(k), new.funcs.get(k)
        if not fo:
            added.append(k)
        elif not fn:
            removed.append(k)
        else:
            no, nn = old.normalise(fo), new.normalise(fn)
            if [t for t, _ in no] == [t for t, _ in nn]:
                same.append(k)
                shifted += fo["start"] != fn["start"]
                if fo["file"] != fn["file"]:
                    moved.append(f"{k} ({fo['file']} -> {fn['file']})")
            else:
                changed.append((k, no, nn))
    oo, on = old.objects(), new.objects()
    o_changed = [k for k in sorted(set(oo) & set(on)) if oo[k][2] != on[k][2]]
    o_added, o_removed = sorted(set(on) - set(oo)), sorted(set(oo) - set(on))

    print(f"functions: {len(same)} identical, {len(changed)} changed, {len(added)} added, "
          f"{len(removed)} removed ({shifted} of the identical ones at another address)")
    print(f"objects (named constants and data): {len(set(oo) & set(on)) - len(o_changed)} "
          f"identical, {len(o_changed)} changed, {len(o_added)} added, {len(o_removed)} removed")
    if moved:
        print(f"moved to another file, identical: {len(moved)}")
        for m in moved:
            print(f"  {m}")
    for label, items in (("added", added), ("removed", removed)):
        if items:
            print(f"{label} functions: " + ", ".join(items))
    for label, items in (("changed", o_changed), ("added", o_added), ("removed", o_removed)):
        if items:
            print(f"{label} objects: " + ", ".join(items))
    if a.all:
        print("identical functions: " + ", ".join(same))
    for k, no, nn in changed:
        d = list(difflib.unified_diff([t for _, t in no], [t for _, t in nn], lineterm="", n=2))[2:]
        print(f"\n== {k} ({len(no) * 4} -> {len(nn) * 4} bytes)")
        for line in d[:a.lines]:
            print("  " + line)
        if len(d) > a.lines:
            print(f"  ... {len(d) - a.lines} more lines")
    sys.exit(1 if changed or added or removed or o_changed or o_added or o_removed else 0)


if __name__ == "__main__":
    main()
