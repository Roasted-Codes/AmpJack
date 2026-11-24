#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""WP0.2 acceptance (docs/PLAN.md): xw.py hooks lists the 4 XInput and 3 XVoiced functions as
ours and nothing foreign; the tee (from run.py) records a deliberate xw.py usbreset 0 and the
controller's re-enumeration; xbdm's screenshot decodes.

  XBOX=10.0.0.13 python3 xbox360/accept/run.py wp0_2
  ... --read-only   everything but the USB reset (the controller on port 0 drops for ~2 s)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from accept import Checks, read_only, tee_log, tee_mark, tee_wait, xw  # noqa: E402

import shot  # noqa: E402
import xw as xwpy  # noqa: E402
from xa import Xbdm  # noqa: E402

c = Checks()

# 1. hooks
res = {r["name"]: r for r in xwpy.hooks_check(Xbdm())}
for name, _, _ in xwpy.HOOK_TARGETS[:7]:
    r = res.get(name)
    c.check(f"hooks: {name} is ours", r and r["kind"] == "ours", r and r["detail"])
vd = res.get("VdSwap")
c.check("hooks: VdSwap is original code or ours", vd and vd["kind"] in ("original", "ours"),
        vd and f"{vd['kind']}: {vd['detail']}")
c.check("hooks: nothing foreign", not [r for r in res.values() if r["kind"] == "foreign"],
        ", ".join(r["name"] for r in res.values() if r["kind"] == "foreign"))
rc, out = xw("hooks")
c.check("xw.py hooks exits 0", rc == 0, out.strip().splitlines()[-1] if out.strip() else "")

# 2. screenshot
try:
    fr, hdr = shot.grab()
    lit = any(any(fr.rgb(x, y)) for y in range(0, fr.height, 16) for x in range(0, fr.width, 16))
    c.check("screenshot decodes", fr.width >= 640 and fr.height >= 480 and lit,
            shot.describe(fr, hdr))
except (OSError, SystemExit) as e:
    c.check("screenshot decodes", False, str(e))

# 3. the tee across a USB reset of port 0
log = tee_log()
c.check("tee running (XW_TEE_LOG)", bool(log), log or "run this through accept/run.py")
if log and not read_only():
    mark = tee_mark()
    rc, out = xw("usbreset", 0)
    c.check("xw.py usbreset 0 sent", rc == 0 and out.startswith("200"), out.strip())
    line = tee_wait(mark, lambda l: "usbreset: port 0" in l and "re-enumerating" in l, 10)
    c.check("tee: the reset is in the log", bool(line), line or "no 'usbreset: port 0 ... "
            "re-enumerating' line within 10 s")
    line = tee_wait(mark, lambda l: "port 0: controller connected" in l, 20)
    c.check("tee: the controller came back (re-enumeration)", bool(line),
            line or "no 'port 0: controller connected' line within 20 s")
    line = tee_wait(mark, lambda l: "CONSOLE SILENT" in l, 0)
    c.check("tee: the console never went silent", not line, line or "")
elif log:
    print("(read-only: usbreset skipped)")
c.done()
