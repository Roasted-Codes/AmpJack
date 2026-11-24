// SPDX-License-Identifier: GPL-3.0-or-later
/* Host test of the rumble decision (xbox360/rumble_360.h, xr_step), a pure function.
 *
 *   cc -O2 -Wall -Wextra -o /tmp/t tests/test_rumble.c && /tmp/t
 *
 * Checks: levels pass through with Vibration on or not known; Vibration off silences new
 * requests and stops running motors (reason "setting") and lets the request through again
 * when switched back on; a stop is sent once more XR_RESTOP_MS later, also while the title
 * repeats its zero every frame; a title switch stops the motors (reason "title") but a request
 * in the same pass wins; the first title seen is not a switch; the test request stops by
 * itself after XR_TEST_MS (gate or not); the gate off is the old behaviour (as asked, nothing
 * forced, no re-sent stop); a repeated request is always passed on; u32 millisecond wrap.
 */
#include <stdio.h>

#include "../xbox360/rumble_360.h"

static int g_fail;

static void check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_fail = 1;
}

static struct xr_pad s;
static struct xr_in in;
static struct xr_out o;

static void pass(u32 dt)
{
	in.now += dt;
	xr_step(&s, &in, &o);
	in.took = in.test = 0;
}

static void ask(u8 l, u8 r)
{
	in.took = 1;
	in.l = l;
	in.r = r;
	pass(16);
}

static void start(u32 now, int gate, int vib)
{
	xr_reset(&s);
	in = (struct xr_in){ .now = now, .title = 0x80001000, .gate = gate, .vib = vib };
	pass(0);
}

int main(void)
{
	/* on: as asked */
	start(1000, 1, 1);
	check(o.act == XR_ACT_NONE, "first pass: nothing to do");
	ask(60, 30);
	check(o.act == XR_ACT_SEND && o.l == 60 && o.r == 30 && o.why == XR_WHY_NONE,
	      "Vibration on: the title's levels");
	ask(60, 30);
	check(o.act == XR_ACT_SEND && o.l == 60, "the same request again is passed on");
	pass(16);
	check(o.act == XR_ACT_NONE, "no request: nothing");
	ask(0, 0);
	check(o.act == XR_ACT_SEND && !o.l && !o.r && o.why == XR_WHY_ASKED, "title's stop");
	pass(50);
	check(o.act == XR_ACT_NONE, "stop not re-sent before 100 ms");
	pass(50);
	check(o.act == XR_ACT_RESEND, "stop re-sent at 100 ms");
	pass(200);
	check(o.act == XR_ACT_NONE, "re-sent once only");

	/* not known (no table): as asked */
	start(1000, 1, -1);
	ask(40, 40);
	check(o.act == XR_ACT_SEND && o.l == 40, "Vibration not known: as asked");

	/* off */
	start(1000, 1, 0);
	ask(80, 80);
	check(o.act == XR_ACT_SEND && !o.l && !o.r && o.zeroed && o.why == XR_WHY_NONE,
	      "Vibration off: a request is silenced (counted), no stop (nothing ran)");
	pass(200);
	check(o.act == XR_ACT_NONE, "off: nothing re-sent when nothing ran");
	in.vib = 1;
	pass(16);
	check(o.act == XR_ACT_SEND && o.l == 80 && o.r == 80,
	      "switched on: the standing request comes through");
	in.vib = 0;
	pass(16);
	check(o.act == XR_ACT_SEND && !o.l && !o.r && o.why == XR_WHY_SETTING && !o.zeroed,
	      "switched off while running: stopped, reason setting");
	for (int i = 0; i < 5; i++)
		ask(80, 80);
	check(o.act != XR_ACT_SEND || (!o.l && !o.r), "off: requests while off stay silent");

	/* restop while the title repeats zero every frame */
	start(1000, 1, 1);
	ask(50, 50);
	ask(0, 0);
	int resent = 0;
	for (int i = 0; i < 10; i++) {
		ask(0, 0);
		resent += o.act == XR_ACT_RESEND;
	}
	check(resent == 1, "stop re-sent once while the title repeats zero");
	ask(50, 50);
	ask(0, 0);
	ask(70, 70);
	pass(200);
	check(o.act == XR_ACT_NONE, "a stop followed by a new level: not re-sent");

	/* title switch */
	start(1000, 1, 1);
	ask(90, 90);
	in.title = 0x80002000;
	pass(16);
	check(o.act == XR_ACT_SEND && !o.l && !o.r && o.why == XR_WHY_TITLE,
	      "title switch: stopped, reason title");
	pass(100);
	check(o.act == XR_ACT_RESEND, "title switch stop re-sent");
	in.title = 0x80003000;
	pass(16);
	check(o.act == XR_ACT_NONE, "title switch with nothing running: nothing");
	ask(20, 20);
	in.title = 0x80004000;
	ask(30, 30);
	check(o.act == XR_ACT_SEND && o.l == 30 && o.why == XR_WHY_NONE,
	      "title switch and a request in the same pass: the request wins");
	xr_reset(&s);
	in.title = 0x80005000;
	ask(10, 10);
	check(o.act == XR_ACT_SEND && o.l == 10, "first title seen is not a switch");

	/* test request times out */
	start(1000, 1, 1);
	in.test = 1;
	ask(50, 0);
	check(o.act == XR_ACT_SEND && o.l == 50, "test request");
	pass(XR_TEST_MS - 100);
	check(o.act == XR_ACT_NONE, "test request still running before its time");
	pass(200);
	check(o.act == XR_ACT_SEND && !o.l && o.why == XR_WHY_TEST, "test request timed out");
	start(1000, 1, 1);
	in.test = 1;
	ask(50, 0);
	ask(40, 40); /* a title's request takes over: no timeout */
	pass(XR_TEST_MS + 100);
	check(o.act == XR_ACT_NONE, "a title's request is never timed out");

	/* gate off: the old behaviour */
	start(1000, 0, 0);
	ask(70, 70);
	check(o.act == XR_ACT_SEND && o.l == 70, "gate off: the setting is not applied");
	in.title = 0x80009000;
	pass(16);
	check(o.act == XR_ACT_NONE, "gate off: title switch forces nothing");
	ask(0, 0);
	check(o.act == XR_ACT_SEND && o.why == XR_WHY_ASKED, "gate off: title's stop");
	pass(200);
	check(o.act == XR_ACT_NONE, "gate off: stop not re-sent");
	in.test = 1;
	ask(30, 30);
	pass(XR_TEST_MS + 100);
	check(o.act == XR_ACT_SEND && !o.l && o.why == XR_WHY_TEST,
	      "gate off: a test request still times out");
	pass(200);
	check(o.act == XR_ACT_NONE, "gate off: its stop not re-sent");

	/* wrap of the millisecond clock */
	start(0xFFFFFFC0u, 1, 1);
	ask(50, 50);
	ask(0, 0); /* restop due after the wrap */
	pass(40);
	check(o.act == XR_ACT_NONE, "wrap: not due yet");
	pass(70);
	check(o.act == XR_ACT_RESEND, "wrap: re-sent when due");

	printf(g_fail ? "FAILED\n" : "all passed\n");
	return g_fail;
}
