# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers for the acceptance scripts, xbox360/accept/<wp>.py (docs/PLAN.md section 0).

A script prints one PASS or FAIL line per check and exits non-zero if any failed; run.py runs
scripts with xw.py tee going and collects the lines. A script starts with

    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from accept import Checks, tee_log, tee_mark, tee_since, xw

so it also runs alone (python3 xbox360/accept/<wp>.py), with XBOX set as for every tool.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
if XBOX360 not in sys.path:
    sys.path.insert(0, XBOX360)


class Checks:
    """c = Checks(); c.check("name", ok, "detail"); ...; c.done()"""

    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""), flush=True)
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        return ok

    def done(self):
        """exit 1 if anything failed or nothing was checked"""
        print(f"{self.passed} passed, {self.failed} failed", flush=True)
        sys.exit(1 if self.failed or not self.passed else 0)


def read_only():
    """ACCEPT_READ_ONLY=1: skip every step that writes to the console (a dry run)"""
    return os.environ.get("ACCEPT_READ_ONLY") == "1"


def tee_log():
    """the log file of the xw.py tee that run.py started (XW_TEE_LOG), or None"""
    p = os.environ.get("XW_TEE_LOG")
    return p if p and os.path.exists(p) else None


def tee_mark():
    """where the tee's log ends now: give it to tee_since later"""
    p = tee_log()
    return os.path.getsize(p) if p else 0


def tee_since(mark):
    """the tee's log lines written after tee_mark() returned mark"""
    p = tee_log()
    if not p:
        return []
    with open(p, "rb") as f:
        f.seek(mark)
        return f.read().decode(errors="replace").splitlines()


def tee_wait(mark, pred, secs):
    """waits up to secs for a tee line after mark for which pred(line) is true: that line,
    or None"""
    until = time.time() + secs
    while True:
        for line in tee_since(mark):
            if pred(line):
                return line
        if time.time() >= until:
            return None
        time.sleep(0.25)


def xw(*args, timeout=120):
    """runs xw.py with these arguments: (exit status, output)"""
    r = subprocess.run([sys.executable, os.path.join(XBOX360, "xw.py"), *map(str, args)],
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr
