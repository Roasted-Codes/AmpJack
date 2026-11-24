#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The surround demo clip's generator, end to end:

    python3 -I tests/test_demo_file.py

Makes the default clip twice into a temporary folder (same bytes both times: reproducible),
checks it with make_demo.py --check (the Python model: every CRC, the peak limit on the
console's maths), then compiles tests/test_demo.c and runs it on the same file (the console's
own reader, surround_demo.h). PASS / FAIL lines; exit status 1 on any failure.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "demo"))

import make_demo  # noqa: E402

fails = 0


def result(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += not ok


with tempfile.TemporaryDirectory() as d:
    a, b = os.path.join(d, "a.demo"), os.path.join(d, "b.demo")
    make_demo.make(a, quiet=True)
    make_demo.make(b, quiet=True)
    result(open(a, "rb").read() == open(b, "rb").read(), "the default clip is the same bytes twice")
    size = os.path.getsize(a)
    result(size < 8 * 1024 * 1024, f"{size} bytes: under 8 MB (six plain channels would be 23 MB)")
    try:
        h, peaks = make_demo.check(a, quiet=True)
        result(True, f"parses: {h['blocks']} blocks, {h['frames'] / 48000:.2f} s; peaks " +
               ", ".join(f"{make_demo.NAMES[c]} {make_demo.dbfs(p)}" for c, p in enumerate(peaks)))
        result(max(peaks) <= make_demo.PEAK_LIMIT, "every channel at or under -6 dBFS")
        result(39.9 <= h["frames"] / 48000 <= 40.1, "about 40 s")
    except ValueError as e:
        result(False, f"make_demo.py --check: {e}")
    exe = os.path.join(d, "test_demo")
    cc = subprocess.run(["cc", "-O2", "-Wall", "-Werror", "-o", exe,
                         os.path.join(HERE, "test_demo.c")], capture_output=True, text=True)
    result(cc.returncode == 0, "tests/test_demo.c compiles" + (": " + cc.stderr if cc.returncode else ""))
    if cc.returncode == 0:
        r = subprocess.run([exe, a], capture_output=True, text=True)
        print(r.stdout, end="")
        result(r.returncode == 0, "the console's reader (surround_demo.h) accepts the clip")
print("FAILED" if fails else "all passed")
sys.exit(1 if fails else 0)
