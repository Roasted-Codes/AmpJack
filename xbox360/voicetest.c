// SPDX-License-Identifier: GPL-3.0-or-later
//
// voicetest.xex: a title that drives a headset the way a game's XHV does, through XAM:
// XamVoiceCreate (xam 780), XamVoiceHeadsetPresent (781), XamVoiceSubmitPacket (782),
// XamVoiceClose (783). For ~10 s it keeps render packets (a generated tone) and capture
// packets queued, polls their status words, then measures the captured PCM: integer
// Goertzel at the tone and at an off-tone frequency, RMS and peak. The tone played into
// the earpiece comes back on the mic (the mic is inside the ear cup), or through xw.xex's
// internal loopback (voice.py set loopback 1).
//
// Settings: Hdd:\voicetest.ini, "key=value" lines (voice.py test writes it):
//   rate=8000     the 360-side sample rate the title assumes (xw.xex must be set the same:
//                 voice.py sets cap_rate/ren_rate before each run)
//   fmt=0         0 s16le, 1 s16be, 2 u8, 3 mu-law (as voice_360.c)
//   freq=1000     tone, Hz
//   seconds=10
//   packet_ms=20  packet length
//   packets=4     packets in flight per direction (xw.xex queues up to 16)
//   user=-1       XAM user; -1 = the first whose voice tag is one of xw.xex's
//   raw=1         also write Hdd:\voicetest.raw: the first 2 s of capture as s16le
//   aurora=1      launch Hdd:\Aurora\Aurora.xex at the end
// Results: Hdd:\voicetest.txt (ends with "end"), and g_vt (magic "VTST", xbdm-readable
// while the title runs). No floating point (not needed in a title, but the analysis is
// integer so it matches what the plugin does).

#include <xecore/xboxkrnl.h>

#define ORD_XamVoiceCreate 780
#define ORD_XamVoiceHeadsetPresent 781
#define ORD_XamVoiceSubmitPacket 782
#define ORD_XamVoiceClose 783
#define ORD_XamLoaderLaunchTitleEx 421

#define DIR_RENDER 0
#define DIR_CAPTURE 1
#define ST_PENDING 0x103
#define OUR_TAG_BASE 0x10000005u

#define MAXPK 8
#define PKT_BYTES_MAX (48000 / 1000 * 40 * 2) /* 40 ms at 48 kHz, 16-bit */
#define RAW_BYTES (48000 * 2 * 2)             /* 2 s at 48 kHz, 16-bit */

#define INI_PATH "\\Device\\Harddisk0\\Partition1\\voicetest.ini"
#define TXT_PATH "\\Device\\Harddisk0\\Partition1\\voicetest.txt"
#define RAW_PATH "\\Device\\Harddisk0\\Partition1\\voicetest.raw"
#define AURORA_DIR "\\Device\\Harddisk0\\Partition1\\Aurora"
#define AURORA_PATH AURORA_DIR "\\Aurora.xex"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;

/* xbdm-readable while running: [0] magic, [1] phase (1 start, 2 resolved, 3 ctx, 4 loop,
 * 5 analysed, 6 written, 7 launching Aurora), [2] heartbeat, [3] user, [4] ctx, [5] tag,
 * [6] present, [7] create status, [8..] see the result file for the full set */
__attribute__((used)) volatile u32 g_vt[32] = { 0x56545354 /* VTST */ };

typedef u32 (*voice_create_t)(u32 user, u32 packets, u32 **ctx);
typedef u32 (*voice_present_t)(u32 *ctx);
typedef u32 (*voice_submit_t)(u32 *ctx, u32 dir, volatile u32 *pkt);
typedef u32 (*voice_close_t)(u32 *ctx);
typedef NTSTATUS (*launch_t)(const char *path, const char *mount, const char *cmdline,
			     uint32_t flags);

/* vt_kcall9(args, fn): a kernel call with nine arguments, the ninth in the 360 ABI's
 * parameter slot at 0x54(r1) (see thunks_common.s, xa_kcall9; NtCreateFile's create
 * options are its ninth argument) */
__asm__("    .text\n"
	"    .globl vt_kcall9\n"
	"    .p2align 2\n"
	"vt_kcall9:\n"
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
long vt_kcall9(const u32 args[9], void *fn);

extern void _start(void);

void vt_entry(void)
{
	g_vt[1] = 1;
	_start();
}

/* ---- settings ---------------------------------------------------------------------------- */

static struct {
	u32 rate, fmt, freq, seconds, packet_ms, packets, raw, aurora;
	s32 user;
} cfg = { 8000, 0, 1000, 10, 20, 4, 1, 1, -1 };

/* ---- time, files, text --------------------------------------------------------------------- */

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

	return (NTSTATUS)vt_kcall9(args, (void *)NtCreateFile);
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

static char g_txt[16384];
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

static void puts32(s32 v)
{
	if (v < 0) {
		put("-");
		putu((u32)-v);
	} else {
		putu((u32)v);
	}
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
	static char buf[1024];
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
		KEY(freq) cfg.freq = (u32)val;
		KEY(seconds) cfg.seconds = (u32)val;
		KEY(packet_ms) cfg.packet_ms = (u32)val;
		KEY(packets) cfg.packets = (u32)val;
		KEY(user) cfg.user = val;
		KEY(raw) cfg.raw = (u32)val;
		KEY(aurora) cfg.aurora = (u32)val;
#undef KEY
	}
}

/* ---- signal ---------------------------------------------------------------------------------- */

/* sin(2 pi i / 256) in Q14 */
static const s16 SINE[256] = {
	0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
	6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
	11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
	15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
	16384, 16379, 16364, 16340, 16305, 16261, 16207, 16143, 16069, 15986, 15893, 15791, 15679, 15557, 15426, 15286,
	15137, 14978, 14811, 14635, 14449, 14256, 14053, 13842, 13623, 13395, 13160, 12916, 12665, 12406, 12140, 11866,
	11585, 11297, 11003, 10702, 10394, 10080, 9760, 9434, 9102, 8765, 8423, 8076, 7723, 7366, 7005, 6639,
	6270, 5897, 5520, 5139, 4756, 4370, 3981, 3590, 3196, 2801, 2404, 2006, 1606, 1205, 804, 402,
	0, -402, -804, -1205, -1606, -2006, -2404, -2801, -3196, -3590, -3981, -4370, -4756, -5139, -5520, -5897,
	-6270, -6639, -7005, -7366, -7723, -8076, -8423, -8765, -9102, -9434, -9760, -10080, -10394, -10702, -11003, -11297,
	-11585, -11866, -12140, -12406, -12665, -12916, -13160, -13395, -13623, -13842, -14053, -14256, -14449, -14635, -14811, -14978,
	-15137, -15286, -15426, -15557, -15679, -15791, -15893, -15986, -16069, -16143, -16207, -16261, -16305, -16340, -16364, -16379,
	-16384, -16379, -16364, -16340, -16305, -16261, -16207, -16143, -16069, -15986, -15893, -15791, -15679, -15557, -15426, -15286,
	-15137, -14978, -14811, -14635, -14449, -14256, -14053, -13842, -13623, -13395, -13160, -12916, -12665, -12406, -12140, -11866,
	-11585, -11297, -11003, -10702, -10394, -10080, -9760, -9434, -9102, -8765, -8423, -8076, -7723, -7366, -7005, -6639,
	-6270, -5897, -5520, -5139, -4756, -4370, -3981, -3590, -3196, -2801, -2404, -2006, -1606, -1205, -804, -402,
};

/* sin of a 32-bit phase (2^32 = one turn), Q14, interpolated */
static s32 isin(u32 ph)
{
	u32 i = ph >> 24, f = (ph >> 16) & 0xff;
	s32 a = SINE[i], b = SINE[(i + 1) & 255];

	return a + (((b - a) * (s32)f) >> 8);
}

/* phase step per sample for f Hz at rate r (f < r/2) */
static u32 phase_step(u32 f, u32 r)
{
	return (f * 65536u / r) << 16 | (((f * 65536u % r) << 16) / r);
}

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

/* Goertzel over blocks of n samples at frequency f; accumulated (power >> 16) */
struct goertzel {
	s32 c; /* 2 cos(w), Q14 */
	s64 s1, s2;
	u32 k, n;
	u64 power;
};

static void gz_init(struct goertzel *g, u32 f, u32 rate, u32 n)
{
	g->c = 2 * isin(phase_step(f, rate) + 0x40000000u);
	g->s1 = g->s2 = 0;
	g->k = 0;
	g->n = n;
	g->power = 0;
}

static void gz_push(struct goertzel *g, s32 x)
{
	s64 s = x + ((g->c * g->s1) >> 14) - g->s2;

	g->s2 = g->s1;
	g->s1 = s;
	if (++g->k == g->n) {
		s64 p = g->s1 * g->s1 + g->s2 * g->s2 - ((g->c * g->s1 >> 7) * g->s2 >> 7);

		g->power += (u64)(p < 0 ? 0 : p) >> 16;
		g->s1 = g->s2 = 0;
		g->k = 0;
	}
}

/* a * 100 / b for u64 values, without a 64-bit division */
static u32 pct64(u64 a, u64 b)
{
	while (a >= 0x01000000u || b >= 0x01000000u) {
		a >>= 1;
		b >>= 1;
	}
	return b ? (u32)(a * 100 / b) : 0;
}

/* bytes over ms as bytes per second, 32-bit */
static u32 per_second(u32 bytes, u32 ms)
{
	return ms ? bytes / ms * 1000 + bytes % ms * 1000 / ms : 0;
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

/* ---- the test -------------------------------------------------------------------------------- */

static u8 g_buf[2][MAXPK][PKT_BYTES_MAX];
static volatile u32 g_pkt[2][MAXPK][8];
static u32 g_inflight[2][MAXPK];
static u8 g_raw[RAW_BYTES];
static u32 g_raw_n;

struct stats {
	u32 submits, submit_errors, first_error, completions, bytes, other_status, first_other;
	u32 first_ms, last_ms, gap_min, gap_max, prev_ms;
};

int main(void)
{
	HMODULE xam = 0;
	voice_create_t create = 0;
	voice_present_t present = 0;
	voice_submit_t submit = 0;
	voice_close_t vclose = 0;
	launch_t launch = 0;
	u32 *ctx = 0, user = 0, tag = 0, create_st = 0, present_r = 0, close_r = 0;
	u32 tags[4] = { 0 }, create_sts[4] = { 0 };
	struct stats st[2];
	u32 rate, bps, pkt_len, ph = 0, step, t0, t_end, now, polls = 0;
	u32 cap_samples = 0, skip, peak = 0, after_close[2] = { 0, 0 };
	u64 energy = 0;
	u8 first64[64];
	u32 first64_n = 0;
	struct goertzel g_on, g_off;
	NTSTATUS wst;

	g_vt[1] = 1;
	__builtin_memset(st, 0, sizeof(st));
	st[0].gap_min = st[1].gap_min = 0xffffffff;
	read_ini();
	if (cfg.rate < 4000 || cfg.rate > 48000)
		cfg.rate = 8000;
	if (cfg.fmt > 3)
		cfg.fmt = 0;
	if (cfg.packet_ms < 2 || cfg.packet_ms > 40)
		cfg.packet_ms = 20;
	if (cfg.packets < 1 || cfg.packets > MAXPK)
		cfg.packets = 4;
	if (cfg.seconds < 1 || cfg.seconds > 120)
		cfg.seconds = 10;
	if (cfg.freq < 100 || cfg.freq * 2 >= cfg.rate)
		cfg.freq = cfg.rate / 8;
	rate = cfg.rate;
	bps = fmt_bytes(cfg.fmt);
	pkt_len = rate / 1000 * cfg.packet_ms * bps;
	step = phase_step(cfg.freq, rate);

	put("voicetest 1\n");
	kv("rate", rate);
	kv("fmt", cfg.fmt);
	kv("freq", cfg.freq);
	kv("seconds", cfg.seconds);
	kv("packet_ms", cfg.packet_ms);
	kv("packet_bytes", pkt_len);
	kv("packets", cfg.packets);

	XexGetModuleHandle("xam.xex", &xam);
	XexGetProcedureAddress(xam, ORD_XamVoiceCreate, (void **)&create);
	XexGetProcedureAddress(xam, ORD_XamVoiceHeadsetPresent, (void **)&present);
	XexGetProcedureAddress(xam, ORD_XamVoiceSubmitPacket, (void **)&submit);
	XexGetProcedureAddress(xam, ORD_XamVoiceClose, (void **)&vclose);
	XexGetProcedureAddress(xam, ORD_XamLoaderLaunchTitleEx, (void **)&launch);
	kx("XamVoiceCreate", (u32)create);
	kx("XamVoiceSubmitPacket", (u32)submit);
	g_vt[1] = 2;
	if (!create || !present || !submit || !vclose) {
		put("error=XamVoice exports not found\n");
		goto done;
	}

	/* the user: the one asked for, or the first whose voice tag is xw.xex's */
	for (u32 u = 0; u < 4; u++) {
		u32 *c = 0;

		if (cfg.user >= 0 && (u32)cfg.user != u)
			continue;
		create_sts[u] = create(u, cfg.packets * 2, &c);
		tags[u] = c ? c[0] : 0;
		put("user");
		putu(u);
		put(": create ");
		putx(create_sts[u]);
		put(" ctx ");
		putx((u32)c);
		put(" tag ");
		putx(tags[u]);
		put("\n");
		if (!create_sts[u] && c && !ctx &&
		    (cfg.user >= 0 || tags[u] - OUR_TAG_BASE < 4)) {
			ctx = c;
			user = u;
			tag = tags[u];
			create_st = create_sts[u];
		} else if (!create_sts[u] && c) {
			vclose(c);
		}
	}
	g_vt[3] = user;
	g_vt[4] = (u32)ctx;
	g_vt[5] = tag;
	g_vt[7] = create_st;
	if (!ctx) {
		put("error=no user with an xw.xex voice tag (headset not bound: voice.py status)\n");
		goto done;
	}
	present_r = present(ctx);
	g_vt[6] = present_r;
	g_vt[1] = 3;
	kv("user", user);
	kx("tag", tag);
	kv("headset_present", present_r);

	gz_init(&g_on, cfg.freq, rate, rate / 50);
	gz_init(&g_off, cfg.freq * 3 / 2 + 37, rate, rate / 50);
	skip = rate; /* the first second: start-up and loop latency */

	t0 = now_ms();
	t_end = t0 + cfg.seconds * 1000;
	g_vt[1] = 4;
	for (now = t0; (s32)(now - t_end) < 0; now = now_ms()) {
		for (u32 d = 0; d < 2; d++) {
			for (u32 i = 0; i < cfg.packets; i++) {
				volatile u32 *p = g_pkt[d][i];
				struct stats *s = &st[d];

				if (g_inflight[d][i]) {
					u32 status = p[0];

					if (status == ST_PENDING)
						continue;
					/* the plugin writes pkt[4] before pkt[0] (lwsync): read in the
					 * opposite order, with the matching barrier */
					__asm__ __volatile__("lwsync" ::: "memory");
					g_inflight[d][i] = 0;
					s->completions++;
					s->bytes += p[1];
					if (!s->first_ms)
						s->first_ms = now;
					if (s->prev_ms) {
						u32 gap = now - s->prev_ms;

						if (gap < s->gap_min)
							s->gap_min = gap;
						if (gap > s->gap_max)
							s->gap_max = gap;
					}
					s->prev_ms = now;
					s->last_ms = now;
					if (status) {
						if (!s->other_status++)
							s->first_other = status;
					} else if (d == DIR_CAPTURE) {
						const u8 *b = g_buf[d][i];
						u32 n = p[1];

						for (u32 k = 0; k < n && first64_n < 64; k++)
							first64[first64_n++] = b[k];
						for (u32 k = 0; k + bps <= n; k += bps) {
							s32 x = get_sample(b + k, cfg.fmt);

							if (g_raw_n + 2 <= RAW_BYTES && cfg.raw &&
							    g_raw_n < rate * 4) {
								g_raw[g_raw_n++] = (u8)x;
								g_raw[g_raw_n++] = (u8)(x >> 8);
							}
							if (cap_samples++ < skip)
								continue;
							if ((x < 0 ? -x : x) > (s32)peak)
								peak = (u32)(x < 0 ? -x : x);
							energy += (u64)(s64)(x * x);
							gz_push(&g_on, x);
							gz_push(&g_off, x);
						}
					}
				}
				/* (re)submit */
				p[2] = (u32)g_buf[d][i];
				p[3] = pkt_len;
				p[4] = 0;
				p[5] = 1; /* XHV's mode field; the wired driver echoes it */
				p[1] = 0;
				p[0] = 0;
				if (d == DIR_RENDER)
					for (u32 k = 0; k + bps <= pkt_len; k += bps, ph += step)
						put_sample(g_buf[d][i] + k, cfg.fmt, (s16)isin(ph));
				else
					__builtin_memset(g_buf[d][i], 0xAA, pkt_len);
				{
					u32 r = submit(ctx, d, p);

					s->submits++;
					if (r) {
						if (!s->submit_errors++)
							s->first_error = r;
					} else {
						g_inflight[d][i] = 1;
					}
				}
			}
		}
		polls++;
		g_vt[2] = polls;
		g_vt[8] = st[1].completions;
		g_vt[9] = st[0].completions;
		sleep_ms(1);
	}
	close_r = vclose(ctx);
	/* every packet still in flight must have been completed by Close */
	for (u32 d = 0; d < 2; d++)
		for (u32 i = 0; i < cfg.packets; i++)
			if (g_inflight[d][i] && g_pkt[d][i][0] != ST_PENDING)
				after_close[d]++;
	g_vt[1] = 5;

	{
		u32 ms = now - t0, analysed = cap_samples > skip ? cap_samples - skip : 0;
		u32 rms = 0;
		u64 tone_ref = 0;

		/* rms = sqrt(energy / n), without a 64-bit division */
		if (analysed) {
			u64 e = energy;
			u32 n = analysed, sh = 0;

			while (e >= 0x80000000u) {
				e >>= 2;
				sh++;
			}
			rms = isqrt64((u64)((u32)e / n)) << sh;
		}
		/* a pure tone of this energy over the blocks: (A N / 2)^2 per block, energy = A^2 N / 2
		 * per block, so Goertzel power / (energy * N / 2) ~ 1 (both >> 16 as accumulated) */
		tone_ref = (energy >> 16) * (rate / 50) / 2;
		kv("duration_ms", ms);
		kv("polls", polls);
		for (u32 d = 0; d < 2; d++) {
			struct stats *s = &st[d];
			const char *n = d == DIR_CAPTURE ? "capture" : "render";

			put(n); put("_submits="); putu(s->submits); put("\n");
			put(n); put("_submit_errors="); putu(s->submit_errors); put(" first ");
			putx(s->first_error); put("\n");
			put(n); put("_completions="); putu(s->completions); put("\n");
			put(n); put("_other_status="); putu(s->other_status); put(" first ");
			putx(s->first_other); put("\n");
			put(n); put("_bytes="); putu(s->bytes); put("\n");
			put(n); put("_first_completion_ms="); putu(s->first_ms ? s->first_ms - t0 : 0);
			put("\n");
			put(n); put("_bytes_per_s=");
			putu(per_second(s->bytes, s->last_ms > s->first_ms ? s->last_ms - s->first_ms : 0));
			put("  (first to last completion)\n");
			put(n); put("_gap_ms="); putu(s->gap_min == 0xffffffff ? 0 : s->gap_min); put("..");
			putu(s->gap_max); put("\n");
			put(n); put("_completed_by_close="); putu(after_close[d]); put("\n");
		}
		kx("close", close_r);
		kv("capture_samples", cap_samples);
		kv("analysed_samples", analysed);
		kv("peak", peak);
		kv("rms", rms);
		kv("tone_pct", pct64(g_on.power, tone_ref));
		kv("offtone_pct", pct64(g_off.power, tone_ref));
		kv("tone_vs_off_x100", pct64(g_on.power, g_off.power + 1));
		put("detected=");
		put(analysed && rms > 100 && pct64(g_on.power, tone_ref) >= 30 &&
			    pct64(g_off.power, tone_ref) < 10 ? "yes" : "no");
		put("\n");
		put("first64=");
		for (u32 k = 0; k < first64_n; k++) {
			static const char hex[] = "0123456789abcdef";
			char c[3] = { hex[first64[k] >> 4], hex[first64[k] & 15], 0 };

			put(c);
		}
		put("\n");
		g_vt[10] = pct64(g_on.power, tone_ref);
		g_vt[11] = pct64(g_off.power, tone_ref);
		g_vt[12] = rms;
		g_vt[13] = per_second(st[1].bytes, st[1].last_ms - st[1].first_ms);
		g_vt[14] = per_second(st[0].bytes, st[0].last_ms - st[0].first_ms);
	}
	(void)puts32;

done:
	if (cfg.raw && g_raw_n) {
		wst = file_write(RAW_PATH, g_raw, g_raw_n);
		kx("raw_write", (u32)wst);
		kv("raw_bytes", g_raw_n);
	}
	put("end\n");
	wst = file_write(TXT_PATH, g_txt, g_txt_n);
	g_vt[15] = (u32)wst;
	g_vt[1] = 6;
	if (cfg.aurora && launch) {
		g_vt[1] = 7;
		sleep_ms(500);
		launch(AURORA_PATH, AURORA_DIR, 0, 0);
	}
	for (;;) {
		sleep_ms(1000);
		g_vt[2]++;
	}
}
