"""Read-only look at the system's own voice / game-volume state (research/DASH-VOICE.md).

    XBOX=10.0.0.13 python3 research/tools/dashvoice.py [seconds]

Only xbdm getmem: nothing on the console is written. Prints, then samples for `seconds`
(default 0) every 100 ms:
  - the kernel ducker in the XAudio render driver (17559: driver object 0x80195F38, fields
    +0x1A0..+0x1C8, read in the dump at 0x8012CA90..0x8012CBC4 and 0x8012D158): its state
    (0 full, 1 ducking down, 2 ducked, 3 coming back), on/off, the gain applied to the last
    frame, its level (= the Guide's Game Volume: 10^(-3 * (10 - N) / 20)), threshold,
    attack, release and hold;
  - XAM's chat manager (global 0x81B2A368): the Game Volume step it last applied (+0x98).
Use it while the owner changes the Guide's voice page or a friend talks: a state other than 0
means the game sound (TV and headset alike) is being turned down right now.
"""
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'xbox360'))
from xa import Xbdm  # noqa: E402

DUCKER = 0x80195F38 + 0x1A0
XAM_CHAT_MANAGER = 0x81B2A368


def f32(b, o):
    return struct.unpack('>f', b[o:o + 4])[0]


def u32(b, o):
    return struct.unpack('>I', b[o:o + 4])[0]


def step_of(level):
    """the Game Volume step 1..10 a ducker level stands for, or None"""
    if level <= 0:
        return None
    n = 10 + 20 * math.log10(level) / 3
    return round(n) if abs(n - round(n)) < 0.05 else None


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 0
    x = Xbdm()
    b = x.mem(DUCKER, 0x2C)
    lvl = f32(b, 0x10)
    print('ducker: state %u on %u gain %.3f level %.4f (%.1f dB, Game Volume %s)' % (
        u32(b, 0), u32(b, 4), f32(b, 0xC), lvl, 20 * math.log10(lvl) if lvl > 0 else -999,
        step_of(lvl)))
    print('        threshold %.5f (mean square), attack %.3f s, release %.3f s, hold %.3f s' % (
        f32(b, 0x1C), f32(b, 0x20), f32(b, 0x24), f32(b, 0x28)))
    m = x.u32s(XAM_CHAT_MANAGER, 1)
    if m and m[0]:
        gv = x.u32s(m[0] + 0x98, 1)
        print('XAM chat manager %08x: Game Volume %s' % (m[0], gv and gv[0]))
    end = time.time() + secs
    last = None
    while time.time() < end:
        b = x.mem(DUCKER, 0x10)
        now = (u32(b, 0), u32(b, 4), round(f32(b, 0xC), 3))
        if now != last:
            print('%7.2f s  state %u on %u gain %.3f' % ((secs - (end - time.time()),) + now))
            last = now
        time.sleep(0.1)
    x.close()


if __name__ == '__main__':
    main()
