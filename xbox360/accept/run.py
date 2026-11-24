#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs acceptance scripts (xbox360/accept/<name>.py) with xw.py tee going, and collects
their PASS / FAIL lines.

  accept/run.py NAME... [--read-only] [--no-tee] [--timeout SECONDS]
      NAME          a script in accept/ (wp0_2, p1_panel, ...; .py optional), or "all"
      --read-only   ACCEPT_READ_ONLY=1 for the scripts: skip the steps that write to the
                    console (each script decides which those are)
      --no-tee      don't start a tee (one is already running elsewhere)
      --timeout S   stop a script that runs longer than S seconds (counts as failed)

Everything goes to out/logs/accept-<date-time>/: the tee's log, tee.out (its screen), and
<name>.out per script. Exit status 1 if any script failed, printed FAIL, or the console went
silent. XBOX=host[:port] as for every tool.
"""
import glob
import os
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
XBOX360 = os.path.dirname(HERE)
OUT = os.path.join(XBOX360, "out")


def scripts(names):
    have = sorted(os.path.basename(p)[:-3] for p in glob.glob(os.path.join(HERE, "*.py"))
                  if os.path.basename(p) not in ("__init__.py", "run.py"))
    if names == ["all"]:
        return have
    out = []
    for n in names:
        n = os.path.basename(n)
        n = n[:-3] if n.endswith(".py") else n
        if n not in have:
            raise SystemExit(f"no accept/{n}.py (there are: {', '.join(have) or 'none'})")
        out.append(n)
    return out


def start_tee(session):
    out = open(os.path.join(session, "tee.out"), "w")
    p = subprocess.Popen([sys.executable, os.path.join(XBOX360, "xw.py"), "tee", session],
                         stdout=out, stderr=subprocess.STDOUT)
    # its log file, once it has found the xw build(s) (or found none)
    until, log = time.time() + 30, None
    while time.time() < until and p.poll() is None:
        logs = glob.glob(os.path.join(session, "*.log"))
        if logs:
            log = logs[0]
            text = open(log, errors="replace").read()
            if " blocks " in text or "no xw build" in text or "CONSOLE SILENT" in text:
                return p, log
        time.sleep(0.25)
    if p.poll() is not None:
        raise SystemExit("xw.py tee ended at once: " + open(os.path.join(session, "tee.out")).read())
    return p, log


def run_one(name, session, env, timeout):
    """runs accept/<name>.py, echoing its output; (exit status, passes, fails)"""
    print(f"==== {name}", flush=True)
    passes = fails = 0
    with open(os.path.join(session, name + ".out"), "w") as out:
        p = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, name + ".py")], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        timer = None
        if timeout:
            timer = threading.Timer(timeout, p.kill)
            timer.start()
        for line in p.stdout:
            print("  " + line, end="", flush=True)
            out.write(line)
            passes += line.startswith("PASS")
            fails += line.startswith("FAIL")
        rc = p.wait()
        if timer:
            timer.cancel()
        if rc < 0:
            print(f"  (stopped after {timeout} s)")
    return rc, passes, fails


def main():
    args = sys.argv[1:]
    opts = {"--read-only": False, "--no-tee": False}
    timeout, names = None, []
    i = 0
    while i < len(args):
        a = args[i]
        if a in opts:
            opts[a] = True
        elif a == "--timeout" and i + 1 < len(args):
            timeout = float(args[i + 1])
            i += 1
        elif a.startswith("--"):
            raise SystemExit(__doc__)
        else:
            names.append(a)
        i += 1
    if not names:
        raise SystemExit(__doc__)
    todo = scripts(names)
    if not os.environ.get("XBOX"):
        raise SystemExit("set XBOX=host[:port] to the console's address")
    session = os.path.join(OUT, "logs", "accept-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(session, exist_ok=True)
    env = dict(os.environ, PYTHONPATH=XBOX360 + os.pathsep + os.environ.get("PYTHONPATH", ""))
    if opts["--read-only"]:
        env["ACCEPT_READ_ONLY"] = "1"
    tee = log = None
    if not opts["--no-tee"]:
        tee, log = start_tee(session)
        if log:
            env["XW_TEE_LOG"] = log
        print(f"tee: {log or '(no log file yet)'}", flush=True)
    results = []
    try:
        for name in todo:
            results.append((name, *run_one(name, session, env, timeout)))
    finally:
        if tee:
            tee.send_signal(signal.SIGINT)
            try:
                tee.wait(10)
            except subprocess.TimeoutExpired:
                tee.kill()
    silent = []
    if log and os.path.exists(log):
        silent = [l for l in open(log, errors="replace") if "CONSOLE SILENT" in l]
    print("==== summary")
    bad = False
    for name, rc, passes, fails in results:
        ok = rc == 0 and not fails and passes
        bad |= not ok
        print(f"{'PASS' if ok else 'FAIL'} {name}: {passes} passed, {fails} failed, exit {rc}")
    for l in silent:
        print("FAIL console went silent: " + l.strip())
    print(f"logs: {session}")
    sys.exit(1 if bad or silent else 0)


if __name__ == "__main__":
    main()
