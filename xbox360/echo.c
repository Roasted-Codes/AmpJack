// SPDX-License-Identifier: GPL-3.0-or-later
//
// echo.xex: a headset test title. It drives the headset through XAM exactly like voicetest.xex
// (XamVoiceCreate 780, XamVoiceHeadsetPresent 781, XamVoiceSubmitPacket 782, XamVoiceClose
// 783), so it goes through xw.xex's virtual 360 headset like a game's XHV would. It never
// generates a tone or any other test signal: the earpiece only ever gets the mic's own audio.
//
// Modes (one at a time), from the user-0 pad (XamInputGetState) or over xbdm (echo.py):
//   A      record: up to 10 s of mic into memory (A again stops early), then saved as
//          Hdd:\echo.raw (s16le mono at the 360-side rate) and Hdd:\echo.txt
//   B      play the last recording into the earpiece once, at unity gain (B again stops)
//   X      echo on/off: the mic back into the earpiece about 0.5 s later, at gain 1/2, with
//          a limiter (below). Off at start; it only starts when asked.
//   Back   stop audio, close the voice context, relaunch Hdd:\Aurora\Aurora.xex
// A short rumble acknowledges each action.
//
// Echo limiter: the mic sits inside the ear cup, so echo can feed back into a howl. The mic
// RMS is measured per capture packet (20 ms). If it keeps rising for ~300 ms (rises without
// a clear drop in between), or stays above LIM_LOUD for ~300 ms, echo is muted for 1 s and
// the delay line is cleared (zeros go in while muted, so the howl can't come back after it).
//
// Settings: Hdd:\echo.ini, "key=value" lines (echo.py launch writes it from xw.xex's applied
// rates): rate=8000 (the 360-side rate, both directions), fmt=0 (0 s16le, 1 s16be, 2 u8,
// 3 mu-law, as voice_360.c), user=-1 (-1 = the first user whose voice tag is xw.xex's),
// aurora=1 (relaunch Aurora on exit).
//
// xbdm control block g_ec (magic "ECHO", see EC_* below). echo.py writes a request word
// (seq << 16 | arg << 8 | op) to g_ec[EC_REQ]; the title handles a request when it differs
// from g_ec[EC_ACK] and then copies it there.
//
// No floating point, no 64-bit division (as voicetest).

#include <xecore/xboxkrnl.h>

#define ORD_XamInputGetState 401
#define ORD_XamInputSetState 402
#define ORD_XamVoiceCreate 780
#define ORD_XamVoiceHeadsetPresent 781
#define ORD_XamVoiceSubmitPacket 782
#define ORD_XamVoiceClose 783
#define ORD_XamLoaderLaunchTitleEx 421

#define DIR_RENDER 0
#define DIR_CAPTURE 1
#define ST_PENDING 0x103
#define OUR_TAG_BASE 0x10000005u

#define PK 4                 /* packets in flight per direction (xw.xex queues up to 16) */
#define PKT_MS 20
#define RATE_MAX 48000
#define PKT_BYTES_MAX (RATE_MAX / 1000 * PKT_MS * 2)
#define REC_SECONDS 10
#define REC_MAX (RATE_MAX * REC_SECONDS) /* samples */
#define FIFO_N RATE_MAX                  /* echo delay line: 1 s at 48 kHz */

#define ECHO_SHIFT 1         /* echo gain 1/2 (>> 1); never above 1 */
#define LIM_RISE_MS 300      /* RMS rising this long: mute */
#define LIM_LOUD 10000       /* or RMS above this ... */
#define LIM_LOUD_MS 300      /* ... this long: mute */
#define LIM_FLOOR 200        /* rises below this RMS don't count (room noise) */
#define LIM_MUTE_MS 1000

#define BTN_BACK 0x0020
#define BTN_A 0x1000
#define BTN_B 0x2000
#define BTN_X 0x4000

#define INI_PATH "\\Device\\Harddisk0\\Partition1\\echo.ini"
#define TXT_PATH "\\Device\\Harddisk0\\Partition1\\echo.txt"
#define RAW_PATH "\\Device\\Harddisk0\\Partition1\\echo.raw"
#define AURORA_DIR "\\Device\\Harddisk0\\Partition1\\Aurora"
#define AURORA_PATH AURORA_DIR "\\Aurora.xex"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;

/* the control/status block (echo.py's FIELDS list follows this order) */
enum {
	EC_MAGIC, EC_PHASE, EC_HEARTBEAT, EC_REQ, EC_ACK, EC_MODE, EC_RATE, EC_FMT,
	EC_USER, EC_CTX, EC_TAG, EC_REC_SAMPLES, EC_REC_PEAK, EC_REC_RMS, EC_SAVES, EC_SAVE_STATUS,
	EC_PLAYS_DONE, EC_PLAY_POS, EC_MIC_RMS, EC_MIC_PEAK, EC_LIM_TRIPS, EC_MUTED, EC_CAP_DONE,
	EC_REN_DONE, EC_SUBMIT_ERRORS, EC_FIRST_ERROR, EC_BUTTONS, EC_FIFO, EC_OTHER_STATUS,
	EC_FIRST_OTHER, EC_CREATE_STATUS, EC_PRESENT, EC_COUNT
};
/* phases: 1 start, 2 exports resolved, 3 waiting for a headset (no voice ctx), 4 running,
 * 5 stopping, 6 closed, 7 launching Aurora */
enum { MODE_IDLE, MODE_RECORD, MODE_PLAY, MODE_ECHO };
enum { OP_NONE, OP_RECORD, OP_STOP, OP_PLAY, OP_ECHO_ON, OP_ECHO_OFF, OP_EXIT };

__attribute__((used)) volatile u32 g_ec[32] = { 0x4543484f /* ECHO */ };

typedef u32 (*voice_create_t)(u32 user, u32 packets, u32 **ctx);
typedef u32 (*voice_present_t)(u32 *ctx);
typedef u32 (*voice_submit_t)(u32 *ctx, u32 dir, volatile u32 *pkt);
typedef u32 (*voice_close_t)(u32 *ctx);
typedef u32 (*input_get_t)(u32 user, u32 flags, u8 *state);
typedef u32 (*input_set_t)(u32 user, u32 flags, u8 *vibration, u32 a, u32 b, u32 c);
typedef NTSTATUS (*launch_t)(const char *path, const char *mount, const char *cmdline,
			     uint32_t flags);

/* ec_kcall9(args, fn): a kernel call with nine arguments, the ninth in the 360 ABI's
 * parameter slot at 0x54(r1) (as voicetest's vt_kcall9; NtCreateFile's ninth argument is its
 * create options) */
__asm__("    .text\n"
	"    .globl ec_kcall9\n"
	"    .p2align 2\n"
	"ec_kcall9:\n"
	"    mflr 0\n"
	"    stwu 1, -112(1)\n"
	"    stw 0, 104(1)\n"
	"    mtctr 4\n"
	"    li 0, 0\n"
	"    stw 0, 0x50(1)\n"
	"    lwz 0, 32(3)\n"
	"    stw 0, 0x54(1)\n"
	"    lwz 4, 4(3)\n"
	"    lwz 5, 8(3)\n"
	"    lwz 6, 12(3)\n"
	"    lwz 7, 16(3)\n"
	"    lwz 8, 20(3)\n"
	"    lwz 9, 24(3)\n"
	"    lwz 10, 28(3)\n"
	"    lwz 3, 0(3)\n"
	"    bctrl\n"
	"    lwz 0, 104(1)\n"
	"    mtlr 0\n"
	"    addi 1, 1, 112\n"
	"    blr\n");
long ec_kcall9(const u32 args[9], void *fn);

extern void _start(void);

void ec_entry(void)
{
	g_ec[EC_PHASE] = 1;
	_start();
}

/* ---- settings ---------------------------------------------------------------------------- */

static struct {
	u32 rate, fmt, aurora;
	s32 user;
} cfg = { 8000, 0, 1, -1 };

/* ---- time, files, text (as voicetest) ----------------------------------------------------- */

static u32 now_ms(void)
{
	int64_t t;

	KeQuerySystemTime(&t);
	return (u32)((u64)t / 10000);
}

static void sleep_ms(u32 ms)
{
	int64_t interval = -(int64_t)ms * 10000;

	KeDelayExecutionThread(1, 0, &interval);
}

static u32 slen(const char *s)
{
	u32 n = 0;

	while (s[n])
		n++;
	return n;
}

/* GENERIC_READ|SYNCHRONIZE FILE_OPEN, or GENERIC_WRITE|SYNCHRONIZE FILE_OVERWRITE_IF;
 * FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE */
static NTSTATUS file_open(HANDLE *h, const char *path, int write)
{
	ANSI_STRING name = { (u16)slen(path), (u16)(slen(path) + 1), (char *)path };
	OBJECT_ATTRIBUTES attrs = { 0, &name, OBJ_CASE_INSENSITIVE };
	IO_STATUS_BLOCK io;
	u32 args[9] = { (u32)h, write ? 0x40100000 : 0x80100000, (u32)&attrs, (u32)&io, 0, 0x80,
			write ? 0 : 1, write ? 5 : 1, 0x60 };

	return (NTSTATUS)ec_kcall9(args, (void *)NtCreateFile);
}

static NTSTATUS file_write(const char *path, const void *data, u32 len)
{
	HANDLE h;
	IO_STATUS_BLOCK io;
	int64_t off = 0;
	NTSTATUS st = file_open(&h, path, 1);

	if ((s32)st < 0)
		return st;
	st = NtWriteFile(h, 0, 0, 0, &io, (void *)data, len, &off);
	NtClose(h);
	return st;
}

static char g_txt[2048];
static u32 g_txt_n;

static void put(const char *s)
{
	while (*s && g_txt_n < sizeof(g_txt) - 1)
		g_txt[g_txt_n++] = *s++;
}

static void putu(u32 v)
{
	char b[12];
	int n = 0;

	do {
		b[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && g_txt_n < sizeof(g_txt) - 1)
		g_txt[g_txt_n++] = b[--n];
}

static void putx(u32 v)
{
	static const char hex[] = "0123456789abcdef";

	for (int i = 28; i >= 0; i -= 4) {
		char c[2] = { hex[(v >> i) & 15], 0 };

		put(c);
	}
}

static void kv(const char *k, u32 v)
{
	put(k);
	put("=");
	putu(v);
	put("\n");
}

static void kx(const char *k, u32 v)
{
	put(k);
	put("=");
	putx(v);
	put("\n");
}

static void read_ini(void)
{
	static char buf[512];
	HANDLE h;
	IO_STATUS_BLOCK io;
	int64_t off = 0;
	u32 n;

	if ((s32)file_open(&h, INI_PATH, 0) < 0)
		return;
	if ((s32)NtReadFile(h, 0, 0, 0, &io, buf, sizeof(buf) - 1, &off) < 0)
		io.Information = 0;
	NtClose(h);
	n = (u32)io.Information;
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
	for (char *p = buf; *p;) {
		char *k = p, *v;
		s32 val = 0, neg = 0;

		while (*p && *p != '=' && *p != '\n')
			p++;
		if (*p != '=') {
			while (*p && *p != '\n')
				p++;
			if (*p)
				p++;
			continue;
		}
		*p++ = 0;
		v = p;
		if (*v == '-') {
			neg = 1;
			v++;
		}
		while (*v >= '0' && *v <= '9')
			val = val * 10 + (*v++ - '0');
		if (neg)
			val = -val;
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
#define KEY(name) else if (k[0] == #name[0] && !__builtin_strcmp(k, #name))
		if (0) {
		}
		KEY(rate) cfg.rate = (u32)val;
		KEY(fmt) cfg.fmt = (u32)val;
		KEY(user) cfg.user = val;
		KEY(aurora) cfg.aurora = (u32)val;
#undef KEY
	}
}

/* ---- samples (as voicetest / voice_360.c) ---------------------------------------------------- */

static u32 fmt_bytes(u32 fmt)
{
	return fmt <= 1 ? 2 : 1;
}

static u8 ulaw_encode(s16 s)
{
	s32 v = s, sign = 0, exp = 7;

	if (v < 0) {
		v = -v;
		sign = 0x80;
	}
	if (v > 32635)
		v = 32635;
	v += 0x84;
	while (exp > 0 && !(v & (0x4000 >> (7 - exp))))
		exp--;
	return (u8)~(sign | exp << 4 | ((v >> (exp + 3)) & 0x0f));
}

static s16 ulaw_decode(u8 u)
{
	s32 v;

	u = (u8)~u;
	v = ((u & 0x0f) << 3) + 0x84;
	v <<= (u >> 4) & 7;
	v -= 0x84;
	return (s16)(u & 0x80 ? -v : v);
}

static void put_sample(u8 *b, u32 fmt, s16 v)
{
	switch (fmt) {
	case 1: b[0] = (u8)((u16)v >> 8); b[1] = (u8)v; break;
	case 2: b[0] = (u8)(((u16)v >> 8) ^ 0x80); break;
	case 3: b[0] = ulaw_encode(v); break;
	default: b[0] = (u8)v; b[1] = (u8)((u16)v >> 8);
	}
}

static s16 get_sample(const u8 *b, u32 fmt)
{
	switch (fmt) {
	case 1: return (s16)(b[0] << 8 | b[1]);
	case 2: return (s16)((b[0] ^ 0x80) << 8);
	case 3: return ulaw_decode(b[0]);
	default: return (s16)(b[0] | b[1] << 8);
	}
}

static s16 clamp16(s32 v)
{
	return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static u32 abs32(s32 v)
{
	return (u32)(v < 0 ? -v : v);
}

static u32 isqrt64(u64 v)
{
	u64 r = 0, bit = (u64)1 << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (u32)r;
}

/* sqrt(energy / n), without a 64-bit division (as voicetest) */
static u32 rms_of(u64 energy, u32 n)
{
	u32 sh = 0;

	if (!n)
		return 0;
	while (energy >= 0x80000000u) {
		energy >>= 2;
		sh++;
	}
	return isqrt64((u64)((u32)energy / n)) << sh;
}

/* ---- state ----------------------------------------------------------------------------------- */

static u8 g_buf[2][PK][PKT_BYTES_MAX];
static volatile u32 g_pkt[2][PK][8];
static u32 g_head[2], g_tail[2]; /* per direction: submitted / taken back (slot = n % PK) */
static u32 g_len[2][PK];         /* bytes submitted per slot */

static s16 g_rec[REC_MAX]; /* native s16; byte-swapped to s16le only while being written */
static u32 g_rec_n, g_rec_max, g_rec_t0, g_rec_cap_bytes, g_rec_short, g_rec_dropped;
static u32 g_play_pos;

static s16 g_fifo[FIFO_N];
static u32 g_fifo_r, g_fifo_n, g_echo_primed;

static struct {
	u32 prev_rms, rise_ms, loud_ms, mute_until, muted;
} lim;

static u32 *g_ctx, g_rate, g_fmt, g_bps, g_pkt_len, g_mode;
static voice_create_t vcreate;
static voice_present_t vpresent;
static voice_submit_t vsubmit;
static voice_close_t vclose;
static input_get_t input_get;
static input_set_t input_set;
static launch_t launch;
static u32 g_rumble_until, g_rumbling;

/* ---- feedback --------------------------------------------------------------------------------- */

static void rumble(u32 on)
{
	/* XINPUT_VIBRATION: two big-endian u16 motor speeds; a light pulse */
	u8 vib[4] = { on ? 0x30 : 0, 0, on ? 0x30 : 0, 0 };

	if (input_set)
		input_set(0, 0, vib, 0, 0, 0);
	g_rumbling = on;
}

static void pulse(void)
{
	rumble(1);
	g_rumble_until = now_ms() + 120;
}

/* ---- the voice context ------------------------------------------------------------------------ */

/* the user asked for, or the first whose voice tag is xw.xex's (as voicetest) */
static void open_voice(void)
{
	for (u32 u = 0; u < 4 && !g_ctx; u++) {
		u32 *c = 0, st;

		if (cfg.user >= 0 && (u32)cfg.user != u)
			continue;
		st = vcreate(u, 2 * PK, &c);
		g_ec[EC_CREATE_STATUS] = st;
		if (!st && c && (cfg.user >= 0 || c[0] - OUR_TAG_BASE < 4)) {
			g_ctx = c;
			g_ec[EC_USER] = u;
			g_ec[EC_TAG] = c[0];
		} else if (!st && c) {
			vclose(c);
		}
	}
	g_ec[EC_CTX] = (u32)g_ctx;
	if (g_ctx) {
		g_ec[EC_PRESENT] = vpresent(g_ctx);
		g_head[0] = g_head[1] = g_tail[0] = g_tail[1] = 0;
	}
}

/* every packet still queued is completed by Close (xw.xex cancels our ctx's packets) */
static void close_voice(void)
{
	if (!g_ctx)
		return;
	vclose(g_ctx);
	g_ctx = 0;
	g_ec[EC_CTX] = 0;
	g_head[0] = g_head[1] = g_tail[0] = g_tail[1] = 0;
}

/* ---- recording -------------------------------------------------------------------------------- */

static void save_recording(void)
{
	u64 energy = 0;
	u32 peak = 0, clipped = 0, n = g_rec_n, ms = now_ms() - g_rec_t0;
	NTSTATUS raw_st = 0, txt_st;

	for (u32 i = 0; i < n; i++) {
		s32 x = g_rec[i];

		if (abs32(x) > peak)
			peak = abs32(x);
		if (x >= 32767 || x <= -32768)
			clipped++;
		energy += (u64)(s64)(x * x);
	}
	g_ec[EC_REC_PEAK] = peak;
	g_ec[EC_REC_RMS] = rms_of(energy, n);

	if (n) {
		u8 *b = (u8 *)g_rec;

		for (u32 i = 0; i < n; i++) { /* to s16le in place */
			u8 t = b[2 * i];

			b[2 * i] = b[2 * i + 1];
			b[2 * i + 1] = t;
		}
		raw_st = file_write(RAW_PATH, g_rec, n * 2);
		for (u32 i = 0; i < n; i++) { /* and back, for playback */
			u8 t = b[2 * i];

			b[2 * i] = b[2 * i + 1];
			b[2 * i + 1] = t;
		}
	}

	g_txt_n = 0;
	put("echo 1\n");
	kv("rate", g_rate);
	put("format=s16le\n");
	put("channels=1\n");
	kv("source_fmt", g_fmt);
	kv("samples", n);
	kv("bytes", n * 2);
	kv("length_ms", g_rate ? n / g_rate * 1000 + n % g_rate * 1000 / g_rate : 0);
	kv("wall_ms", ms);
	kv("peak", peak);
	kv("rms", g_ec[EC_REC_RMS]);
	kv("clipped", clipped);
	kv("capture_bytes", g_rec_cap_bytes);
	kv("short_packets", g_rec_short);
	kv("dropped_samples", g_rec_dropped);
	kv("user", g_ec[EC_USER]);
	kx("tag", g_ec[EC_TAG]);
	kx("raw_write", (u32)raw_st);
	put("end\n");
	txt_st = file_write(TXT_PATH, g_txt, g_txt_n);
	g_ec[EC_SAVE_STATUS] = (s32)raw_st < 0 ? (u32)raw_st : (u32)txt_st;
	g_ec[EC_SAVES]++;
}

/* ---- modes ------------------------------------------------------------------------------------ */

static void fifo_clear(void)
{
	g_fifo_r = g_fifo_n = 0;
	g_echo_primed = 0;
	g_ec[EC_FIFO] = 0;
}

static void set_mode(u32 m)
{
	if (g_mode == MODE_RECORD && m != MODE_RECORD)
		save_recording(); /* the file write stalls the loop; the plugin drops mic meanwhile */
	if (m == MODE_RECORD) {
		g_rec_n = g_rec_cap_bytes = g_rec_short = g_rec_dropped = 0;
		g_rec_t0 = now_ms();
		g_ec[EC_REC_PEAK] = g_ec[EC_REC_RMS] = g_ec[EC_REC_SAMPLES] = 0;
	}
	if (m == MODE_PLAY)
		g_play_pos = 0;
	if (m == MODE_ECHO) {
		fifo_clear();
		lim.prev_rms = lim.rise_ms = lim.loud_ms = lim.muted = 0;
		g_ec[EC_MUTED] = 0;
	}
	g_mode = m;
	g_ec[EC_MODE] = m;
}

static void start_record(u32 secs)
{
	if (!secs || secs > REC_SECONDS)
		secs = REC_SECONDS;
	set_mode(MODE_IDLE);
	g_rec_max = g_rate * secs;
	set_mode(MODE_RECORD);
}

/* ---- per-packet work -------------------------------------------------------------------------- */

static void limiter_block(u32 rms, u32 now)
{
	if (lim.muted) {
		if ((s32)(now - lim.mute_until) >= 0) {
			lim.muted = 0;
			lim.rise_ms = lim.loud_ms = 0;
			lim.prev_rms = rms;
			g_ec[EC_MUTED] = 0;
		}
		return;
	}
	/* rising: clearly above the last block; a clear drop restarts the count */
	if (rms > LIM_FLOOR && rms > lim.prev_rms + lim.prev_rms / 16 + 8)
		lim.rise_ms += PKT_MS;
	else if (rms + rms / 8 < lim.prev_rms || rms <= LIM_FLOOR)
		lim.rise_ms = 0;
	lim.loud_ms = rms > LIM_LOUD ? lim.loud_ms + PKT_MS : 0;
	lim.prev_rms = rms;
	if (lim.rise_ms >= LIM_RISE_MS || lim.loud_ms >= LIM_LOUD_MS) {
		lim.muted = 1;
		lim.mute_until = now + LIM_MUTE_MS;
		g_ec[EC_MUTED] = 1;
		g_ec[EC_LIM_TRIPS]++;
		fifo_clear();
	}
}

/* n samples of mic */
static void mic_block(const s16 *s, u32 n, u32 now)
{
	u64 energy = 0;
	u32 peak = 0, rms;

	for (u32 i = 0; i < n; i++) {
		s32 x = s[i];

		energy += (u64)(s64)(x * x);
		if (abs32(x) > peak)
			peak = abs32(x);
	}
	rms = rms_of(energy, n);
	g_ec[EC_MIC_RMS] = rms;
	g_ec[EC_MIC_PEAK] = peak;

	if (g_mode == MODE_RECORD) {
		for (u32 i = 0; i < n; i++) {
			if (g_rec_n < g_rec_max)
				g_rec[g_rec_n++] = s[i];
			else
				g_rec_dropped++;
		}
		g_ec[EC_REC_SAMPLES] = g_rec_n;
	} else if (g_mode == MODE_ECHO) {
		limiter_block(rms, now);
		for (u32 i = 0; i < n; i++) {
			s16 x = lim.muted ? 0 : s[i];

			if (g_fifo_n == FIFO_N) { /* full (render stalled): drop the oldest */
				g_fifo_r = (g_fifo_r + 1) % FIFO_N;
				g_fifo_n--;
			}
			g_fifo[(g_fifo_r + g_fifo_n) % FIFO_N] = x;
			g_fifo_n++;
		}
		/* start the earpiece once ~0.4 s is buffered; the packets in flight both ways
		 * (PK x 20 ms each) and the plugin's own buffering bring it to ~0.5 s */
		if (g_fifo_n >= g_rate * 2 / 5)
			g_echo_primed = 1;
		g_ec[EC_FIFO] = g_fifo_n;
	}
}

/* fill render packet buffer b; returns its length in bytes, 0 = nothing to play */
static u32 render_fill(u8 *b)
{
	u32 samples = g_pkt_len / g_bps, n = 0;

	if (g_mode == MODE_PLAY) {
		for (; n < samples && g_play_pos < g_rec_n; n++)
			put_sample(b + n * g_bps, g_fmt, g_rec[g_play_pos++]); /* unity gain */
		g_ec[EC_PLAY_POS] = g_play_pos;
		return n * g_bps;
	}
	if (g_mode == MODE_ECHO) {
		for (; n < samples; n++) {
			s32 x = 0;

			if (g_echo_primed && g_fifo_n) {
				x = g_fifo[g_fifo_r];
				g_fifo_r = (g_fifo_r + 1) % FIFO_N;
				g_fifo_n--;
			}
			if (lim.muted)
				x = 0;
			put_sample(b + n * g_bps, g_fmt, clamp16(x >> ECHO_SHIFT));
		}
		g_ec[EC_FIFO] = g_fifo_n;
		return n * g_bps;
	}
	return 0;
}

/* Each direction uses its PK slots as a ring: packets are submitted in slot order (head) and
 * xw.xex completes them in submit order, so they are taken back strictly from the oldest
 * (tail). That keeps the mic and earpiece streams in order. */
static int submit_one(u32 d, u32 len)
{
	u32 i = g_head[d] % PK, r;
	volatile u32 *p = g_pkt[d][i];

	p[2] = (u32)g_buf[d][i];
	p[3] = len;
	p[4] = 0;
	p[5] = 1; /* XHV's mode field (as voicetest) */
	p[1] = 0;
	p[0] = 0;
	r = vsubmit(g_ctx, d, p);
	if (r) {
		if (!g_ec[EC_SUBMIT_ERRORS]++)
			g_ec[EC_FIRST_ERROR] = r;
		return 0;
	}
	g_len[d][i] = len;
	g_head[d]++;
	return 1;
}

static void capture_done(u32 i, u32 status, u32 now)
{
	static s16 s[RATE_MAX / 1000 * PKT_MS];
	u32 got, n = 0;

	if (status) {
		/* cancelled, or the inactive-process path (full length, nothing written): the
		 * buffer is never used */
		if (!g_ec[EC_OTHER_STATUS]++)
			g_ec[EC_FIRST_OTHER] = status;
		return;
	}
	got = g_pkt[DIR_CAPTURE][i][1];
	if (got > g_len[DIR_CAPTURE][i])
		got = g_len[DIR_CAPTURE][i];
	g_ec[EC_CAP_DONE]++;
	if (g_mode == MODE_RECORD) {
		g_rec_cap_bytes += got;
		if (got < g_len[DIR_CAPTURE][i])
			g_rec_short++;
	}
	for (u32 k = 0; k + g_bps <= got; k += g_bps)
		s[n++] = get_sample(g_buf[DIR_CAPTURE][i] + k, g_fmt);
	mic_block(s, n, now);
}

/* completed packets handled, free slots refilled; returns render packets still in flight */
static u32 service(u32 now)
{
	for (u32 d = 0; d < 2; d++) {
		while (g_tail[d] != g_head[d]) {
			u32 i = g_tail[d] % PK, status = g_pkt[d][i][0];

			if (status == ST_PENDING)
				break;
			/* the plugin writes pkt[4] before pkt[0] (lwsync): read in the opposite
			 * order, with the matching barrier */
			__asm__ __volatile__("lwsync" ::: "memory");
			g_tail[d]++;
			if (d == DIR_CAPTURE)
				capture_done(i, status, now);
			else if (status) {
				if (!g_ec[EC_OTHER_STATUS]++)
					g_ec[EC_FIRST_OTHER] = status;
			} else {
				g_ec[EC_REN_DONE]++;
			}
		}
		while (g_head[d] - g_tail[d] < PK) {
			u32 i = g_head[d] % PK, len;

			if (d == DIR_CAPTURE) {
				/* zeroed, so a short completion can't leave old data behind */
				__builtin_memset(g_buf[d][i], 0, g_pkt_len);
				len = g_pkt_len;
			} else {
				len = render_fill(g_buf[d][i]);
				if (!len)
					break; /* nothing to play: the plugin plays silence */
			}
			if (!submit_one(d, len))
				break; /* (render: those samples are lost; counted as an error) */
		}
	}
	return g_head[DIR_RENDER] - g_tail[DIR_RENDER];
}

/* ---- requests ----------------------------------------------------------------------------------- */

static int g_exit;

static void request(u32 op, u32 arg)
{
	switch (op) {
	case OP_RECORD:
		start_record(arg);
		break;
	case OP_STOP:
	case OP_ECHO_OFF:
		if (op == OP_STOP || g_mode == MODE_ECHO)
			set_mode(MODE_IDLE);
		break;
	case OP_PLAY:
		set_mode(MODE_IDLE);
		if (g_rec_n)
			set_mode(MODE_PLAY);
		break;
	case OP_ECHO_ON:
		set_mode(MODE_IDLE);
		set_mode(MODE_ECHO);
		break;
	case OP_EXIT:
		g_exit = 1;
		break;
	default:
		return;
	}
	pulse();
}

static void buttons(u32 *last)
{
	u8 st[16]; /* XINPUT_STATE: dwPacketNumber, XINPUT_GAMEPAD (wButtons first) */
	u32 b, down;

	__builtin_memset(st, 0, sizeof(st));
	if (!input_get || input_get(0, 1 /* XINPUT_FLAG_GAMEPAD */, st))
		b = 0;
	else
		b = (u32)(st[4] << 8 | st[5]);
	g_ec[EC_BUTTONS] = b;
	down = b & ~*last;
	*last = b;
	if (down & BTN_BACK)
		request(OP_EXIT, 0);
	else if (down & BTN_A)
		request(g_mode == MODE_RECORD ? OP_STOP : OP_RECORD, 0);
	else if (down & BTN_B)
		request(g_mode == MODE_PLAY ? OP_STOP : OP_PLAY, 0);
	else if (down & BTN_X)
		request(g_mode == MODE_ECHO ? OP_ECHO_OFF : OP_ECHO_ON, 0);
}

int main(void)
{
	HMODULE xam = 0;
	u32 last_buttons = 0, next_input = 0, next_open = 0, polls = 0, fail_passes = 0;

	g_ec[EC_PHASE] = 1;
	read_ini();
	if (cfg.rate < 4000 || cfg.rate > RATE_MAX)
		cfg.rate = 8000;
	if (cfg.fmt > 3)
		cfg.fmt = 0;
	g_rate = cfg.rate;
	g_fmt = cfg.fmt;
	g_bps = fmt_bytes(g_fmt);
	g_pkt_len = g_rate / 1000 * PKT_MS * g_bps;
	g_ec[EC_RATE] = g_rate;
	g_ec[EC_FMT] = g_fmt;

	XexGetModuleHandle("xam.xex", &xam);
	XexGetProcedureAddress(xam, ORD_XamVoiceCreate, (void **)&vcreate);
	XexGetProcedureAddress(xam, ORD_XamVoiceHeadsetPresent, (void **)&vpresent);
	XexGetProcedureAddress(xam, ORD_XamVoiceSubmitPacket, (void **)&vsubmit);
	XexGetProcedureAddress(xam, ORD_XamVoiceClose, (void **)&vclose);
	XexGetProcedureAddress(xam, ORD_XamInputGetState, (void **)&input_get);
	XexGetProcedureAddress(xam, ORD_XamInputSetState, (void **)&input_set);
	XexGetProcedureAddress(xam, ORD_XamLoaderLaunchTitleEx, (void **)&launch);
	g_ec[EC_PHASE] = 2;

	while (!g_exit) {
		u32 now = now_ms(), req = g_ec[EC_REQ];

		if (req != g_ec[EC_ACK]) {
			request(req & 0xff, (req >> 8) & 0xff);
			g_ec[EC_ACK] = req;
		}
		if ((s32)(now - next_input) >= 0) {
			buttons(&last_buttons);
			next_input = now + 16;
		}
		if (g_rumbling && (s32)(now - g_rumble_until) >= 0)
			rumble(0);

		if (!g_ctx && vcreate && vpresent && vsubmit && vclose &&
		    (s32)(now - next_open) >= 0) {
			open_voice();
			next_open = now + 2000;
		}
		g_ec[EC_PHASE] = g_ctx ? 4 : 3;
		if (g_ctx) {
			u32 errors = g_ec[EC_SUBMIT_ERRORS];
			u32 ren_busy = service(now);

			/* a playback ends once every render packet has been consumed */
			if (g_mode == MODE_PLAY && g_play_pos >= g_rec_n && !ren_busy) {
				g_ec[EC_PLAYS_DONE]++;
				set_mode(MODE_IDLE);
			}
			if (g_mode == MODE_RECORD && g_rec_n >= g_rec_max)
				set_mode(MODE_IDLE); /* saves */
			/* the headset went away (submits failing for ~100 ms): reopen in 2 s */
			fail_passes = g_ec[EC_SUBMIT_ERRORS] != errors ? fail_passes + 1 : 0;
			if (fail_passes >= 100) {
				fail_passes = 0;
				if (g_mode == MODE_RECORD)
					set_mode(MODE_IDLE);
				close_voice();
				next_open = now + 2000;
			}
		}
		g_ec[EC_HEARTBEAT] = ++polls;
		sleep_ms(1);
	}

	g_ec[EC_PHASE] = 5;
	set_mode(MODE_IDLE); /* a recording in progress is saved */
	close_voice();
	rumble(0);
	g_ec[EC_PHASE] = 6;
	if (cfg.aurora && launch) {
		g_ec[EC_PHASE] = 7;
		sleep_ms(500);
		launch(AURORA_PATH, AURORA_DIR, 0, 0);
	}
	for (;;) {
		sleep_ms(1000);
		g_ec[EC_HEARTBEAT]++;
	}
}
