// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * xwired: drives a wired Xbox One / Series controller (any GIP pad: interface FF/47/D0)
 * over libusb with the same core/gip.c the Xbox 360 plugin uses, and prints its input.
 * Runs on macOS and Linux. It's the quick way to check a controller before the console.
 *
 *   make -C desktop && ./desktop/xwired          (sudo may be needed to claim the pad)
 *
 * XA_DEBUG=1 logs every GIP message but input, 2 adds input, 3 adds raw USB dumps.
 * XWIRED_AUTH=1 also runs the security handshake (the 360 plugin doesn't by default).
 * The triggers and A/B drive the rumble motors, to check rumble.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include <libusb.h>

#include "../core/gip.h"
#include "../core/platform.h"

#define GIP_CLASS 0xff
#define GIP_SUBCLASS 0x47
#define GIP_PROTOCOL 0xd0
#define PKT_LEN 64

int xa_debug;

/* ---- platform.h (the wireless adapter's parts are unused here) ---------------------- */

struct xa_usb;
int xa_usb_ctrl(struct xa_usb *usb, u8 request_type, u8 request, u16 value, u16 index,
		void *data, u16 len)
{
	return -XA_EIO;
}

int xa_usb_bulk_out(struct xa_usb *usb, u8 ep, const void *data, int len)
{
	return -XA_EIO;
}

int xa_random(void *buf, int len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	size_t n = f ? fread(buf, 1, len, f) : 0;

	if (f)
		fclose(f);
	return n == (size_t)len ? 0 : -XA_EIO;
}

void xa_sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

u64 xa_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* every line also goes to xwired.log, synced to disk at once: if the Mac panics, the log
 * still shows the last step (a panic loses what is only in the page cache) */
static int log_fd = -1;

void xa_log(const char *fmt, ...)
{
	static u64 start;
	char line[512];
	int n;
	va_list ap;

	if (!start)
		start = xa_now_ms();
	n = snprintf(line, sizeof(line), "[%7.3f] ", (xa_now_ms() - start) / 1000.0);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	n = (int)strlen(line);
	fputs(line, stderr);
	if (log_fd >= 0 && write(log_fd, line, n) == n)
		fsync(log_fd);
}

static void dump(const char *what, const u8 *d, int n)
{
	char line[3 * PKT_LEN + 1] = "";

	for (int i = 0; i < n && i < PKT_LEN; i++)
		snprintf(line + 3 * i, 4, "%02x ", d[i]);
	xa_log("usb: %s %2d: %s\n", what, n, line);
}

/* ---- the controller ------------------------------------------------------------------ */

static libusb_device_handle *handle;
static u8 ep_in, ep_out;

static int wired_send(void *ctx, const u8 *data, int len)
{
	u8 buf[PKT_LEN];
	int done = 0, ret;

	if (len > PKT_LEN)
		return -XA_EINVAL;
	memcpy(buf, data, len);
	if (xa_debug > 2)
		dump("out", buf, len);
	ret = libusb_interrupt_transfer(handle, ep_out, buf, len, &done, 1000);
	if (ret) {
		xa_log("usb: out failed: %s\n", libusb_error_name(ret));
		return ret == LIBUSB_ERROR_NO_DEVICE ? -XA_ENODEV : -XA_EIO;
	}
	return 0;
}

/* ---- headset audio: interface 1, isochronous IN 0x83 / OUT 0x03 ------------------------ */

#define AUDIO_IF 1
#define AUDIO_EP_IN 0x83
#define AUDIO_EP_OUT 0x03
#define AUDIO_FRAMES 8      /* one 8 ms block per transfer, as xone */
#define AUDIO_IN_XFERS 2
#define AUDIO_OUT_MAX 4     /* earpiece blocks in flight */

static struct libusb_transfer *audio_in[AUDIO_IN_XFERS];
static volatile int audio_on, audio_in_inflight, audio_out_inflight;
static unsigned int audio_in_errors, audio_out_errors, audio_in_dropped;
static unsigned int audio_in_maxp = 64, audio_out_maxp; /* alt 1's iso packet sizes */
static bool audio_blocked; /* XWIRED_NOAUDIO: the headset is logged but never streamed */
static FILE *mic_wav;
static unsigned int mic_bytes;

/*
 * Isochronous callbacks run inside libusb's event handling. They only copy the packets
 * into this queue and resubmit: the GIP code (which may switch the alternate setting or
 * cancel transfers) runs from the main loop, outside any callback.
 */
#define RXQ_LEN 64
static struct { u8 data[1023]; u16 len; } rxq[RXQ_LEN];
static unsigned int rxq_w, rxq_r;

static void audio_in_done(struct libusb_transfer *t)
{
	audio_in_inflight--;
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		for (int i = 0; i < t->num_iso_packets; i++) {
			struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
			unsigned int n = d->actual_length;

			if (d->status != LIBUSB_TRANSFER_COMPLETED) {
				audio_in_errors++;
				continue;
			}
			if (!n)
				continue;
			if (n > sizeof(rxq[0].data) || rxq_w - rxq_r >= RXQ_LEN) {
				audio_in_dropped++;
				continue;
			}
			memcpy(rxq[rxq_w % RXQ_LEN].data, libusb_get_iso_packet_buffer_simple(t, i), n);
			rxq[rxq_w % RXQ_LEN].len = (u16)n;
			rxq_w++;
		}
	} else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
		audio_in_errors++;
	}
	if (audio_on && t->status != LIBUSB_TRANSFER_CANCELLED &&
	    t->status != LIBUSB_TRANSFER_NO_DEVICE) {
		if (!libusb_submit_transfer(t))
			audio_in_inflight++;
		else
			audio_in_errors++;
	}
}

/*
 * Interrupt IN: two transfers stay posted all the time, so the pipe is never aborted
 * (a synchronous read with a timeout aborts the pipe every time it times out).
 */
#define INT_IN_XFERS 2
static struct libusb_transfer *int_in[INT_IN_XFERS];
static int int_in_inflight;
static volatile int int_in_gone; /* the controller was unplugged */
static unsigned int int_in_packets, int_in_errors, int_in_dropped;
static struct { u8 data[PKT_LEN]; u8 len; } inq[RXQ_LEN];
static unsigned int inq_w, inq_r;
static volatile sig_atomic_t quit;

static void int_in_done(struct libusb_transfer *t)
{
	int_in_inflight--;
	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		int_in_packets++;
		if (t->actual_length > 0 && t->actual_length <= PKT_LEN && inq_w - inq_r < RXQ_LEN) {
			memcpy(inq[inq_w % RXQ_LEN].data, t->buffer, t->actual_length);
			inq[inq_w % RXQ_LEN].len = (u8)t->actual_length;
			inq_w++;
		} else if (t->actual_length > 0) {
			int_in_dropped++;
		}
	} else if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
		int_in_gone = 1;
		return;
	} else if (t->status == LIBUSB_TRANSFER_CANCELLED) {
		return;
	} else {
		int_in_errors++;
	}
	if (quit)
		return;
	if (!libusb_submit_transfer(t))
		int_in_inflight++;
	else
		int_in_gone = 1;
}

static int int_in_start(void)
{
	static u8 bufs[INT_IN_XFERS][PKT_LEN];

	for (int i = 0; i < INT_IN_XFERS; i++) {
		int ret;

		int_in[i] = libusb_alloc_transfer(0);
		if (!int_in[i])
			return LIBUSB_ERROR_NO_MEM;
		libusb_fill_interrupt_transfer(int_in[i], handle, ep_in, bufs[i], PKT_LEN,
					       int_in_done, NULL, 0 /* no timeout: never aborted */);
		ret = libusb_submit_transfer(int_in[i]);
		if (ret)
			return ret;
		int_in_inflight++;
	}
	return 0;
}

static void int_in_stop(void)
{
	u64 end = xa_now_ms() + 1000;

	for (int i = 0; i < INT_IN_XFERS; i++)
		if (int_in[i])
			libusb_cancel_transfer(int_in[i]);
	while (int_in_inflight > 0 && xa_now_ms() < end) {
		struct timeval tv = { 0, 10000 };

		libusb_handle_events_timeout_completed(NULL, &tv, NULL);
	}
	if (int_in_inflight > 0) {
		xa_log("usb: input transfers still in flight at exit\n");
		return; /* don't free what the kernel may still write into */
	}
	for (int i = 0; i < INT_IN_XFERS; i++)
		if (int_in[i])
			libusb_free_transfer(int_in[i]);
}

/* the queued headset packets, from the main loop */
static void audio_rx_drain(struct gip_client *gc)
{
	while (rxq_r != rxq_w) {
		unsigned int i = rxq_r % RXQ_LEN;

		if (xa_debug > 2)
			dump("aud", rxq[i].data, rxq[i].len);
		gip_rx(gc, rxq[i].data, rxq[i].len);
		rxq_r++;
	}
}

/* waits (handling events, so cancellations complete) until no iso transfer is in flight.
 * Called from the main loop only, never from a callback. */
static bool audio_wait_idle(unsigned int ms)
{
	u64 end = xa_now_ms() + ms;

	while ((audio_in_inflight > 0 || audio_out_inflight > 0) && xa_now_ms() < end) {
		struct timeval tv = { 0, 10000 };

		libusb_handle_events_timeout_completed(NULL, &tv, NULL);
	}
	return audio_in_inflight <= 0 && audio_out_inflight <= 0;
}

static int wired_enable_audio(void *ctx, bool on)
{
	int ret;

	if (!on) {
		if (!audio_on && audio_in_inflight <= 0 && audio_out_inflight <= 0)
			return 0;
		audio_on = 0;
		for (int i = 0; i < AUDIO_IN_XFERS; i++)
			if (audio_in[i])
				libusb_cancel_transfer(audio_in[i]);
		/* never change the alternate setting under transfers still in flight */
		if (!audio_wait_idle(1000)) {
			xa_log("usb: audio transfers still in flight (in %d, out %d): leaving "
			       "interface 1 as it is\n", audio_in_inflight, audio_out_inflight);
			return -XA_EIO;
		}
		ret = libusb_set_interface_alt_setting(handle, AUDIO_IF, 0);
		xa_log("usb: audio off, interface 1 to alternate 0: %s\n",
		       ret ? libusb_error_name(ret) : "ok");
		return 0;
	}
	if (audio_blocked) {
		xa_log("usb: audio not started (XWIRED_NOAUDIO)\n");
		return -XA_EIO;
	}
	if (audio_on)
		return 0;
	if (!audio_wait_idle(1000)) {
		xa_log("usb: old audio transfers still in flight: not starting audio\n");
		return -XA_EIO;
	}
	ret = libusb_set_interface_alt_setting(handle, AUDIO_IF, 1);
	xa_log("usb: audio interface 1 to alternate 1: %s\n", ret ? libusb_error_name(ret) : "ok");
	if (ret)
		return -XA_EIO;
	audio_on = 1;
	for (int i = 0; i < AUDIO_IN_XFERS; i++) {
		struct libusb_transfer *t = audio_in[i];
		static u8 bufs[AUDIO_IN_XFERS][AUDIO_FRAMES * 1023];

		if (!t)
			t = audio_in[i] = libusb_alloc_transfer(AUDIO_FRAMES);
		if (!t)
			return -XA_ENOMEM;
		libusb_fill_iso_transfer(t, handle, AUDIO_EP_IN, bufs[i],
					 (int)(AUDIO_FRAMES * audio_in_maxp), AUDIO_FRAMES,
					 audio_in_done, NULL, 1000);
		libusb_set_iso_packet_lengths(t, audio_in_maxp);
		ret = libusb_submit_transfer(t);
		if (ret) {
			xa_log("usb: audio IN submit: %s\n", libusb_error_name(ret));
			wired_enable_audio(ctx, false);
			return -XA_EIO;
		}
		audio_in_inflight++;
	}
	return 0;
}

static void audio_out_done(struct libusb_transfer *t)
{
	if (t->status != LIBUSB_TRANSFER_COMPLETED)
		audio_out_errors++;
	audio_out_inflight--;
	free(t->buffer);
	libusb_free_transfer(t);
}

static int wired_send_audio(void *ctx, const u8 *data, int frame_len, int frames)
{
	struct libusb_transfer *t;
	u8 *buf;

	if (!audio_on || audio_out_inflight >= AUDIO_OUT_MAX)
		return 0; /* behind: drop a block rather than lag */
	if (audio_out_maxp && (unsigned int)frame_len > audio_out_maxp) {
		static bool said;

		if (!said)
			xa_log("usb: earpiece packet %d bytes > endpoint's %u: not sent\n", frame_len,
			       audio_out_maxp);
		said = true;
		audio_out_errors++;
		return -XA_EINVAL;
	}
	t = libusb_alloc_transfer(frames);
	buf = malloc(frame_len * frames);
	if (!t || !buf)
		return -XA_ENOMEM;
	memcpy(buf, data, frame_len * frames);
	libusb_fill_iso_transfer(t, handle, AUDIO_EP_OUT, buf, frame_len * frames, frames,
				 audio_out_done, NULL, 1000);
	libusb_set_iso_packet_lengths(t, frame_len);
	if (libusb_submit_transfer(t)) {
		audio_out_errors++;
		free(buf);
		libusb_free_transfer(t);
		return -XA_EIO;
	}
	audio_out_inflight++;
	return 0;
}

static struct gip_transport transport = {
	.send = wired_send,
	.no_auth = true,
	.wired = true,
	.enable_audio = wired_enable_audio,
	.send_audio = wired_send_audio,
};

/* mic -> WAV file, and a small ring for the loopback mode */
static s16 loop_ring[48000];
static unsigned int loop_w, loop_r;

static void wav_header(FILE *f, unsigned int rate, unsigned int ch, unsigned int bytes)
{
	u8 h[44] = "RIFF....WAVEfmt ";
	unsigned int v;

#define LE32(o, x) (v = (x), h[o] = v, h[o + 1] = v >> 8, h[o + 2] = v >> 16, h[o + 3] = v >> 24)
	LE32(4, 36 + bytes);
	LE32(16, 16);
	h[20] = 1, h[21] = 0, h[22] = ch, h[23] = 0;
	LE32(24, rate);
	LE32(28, rate * ch * 2);
	h[32] = ch * 2, h[33] = 0, h[34] = 16, h[35] = 0;
	memcpy(h + 36, "data", 4);
	LE32(40, bytes);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
	fseek(f, 0, SEEK_END);
}

static void hs_changed(struct gip_client *gc, bool present)
{
	if (present) {
		mic_wav = fopen("mic.wav", "wb");
		if (mic_wav)
			wav_header(mic_wav, gc->hs.rate_in, gc->hs.channels_in, 0);
		xa_log("headset: ready. Recording the mic to mic.wav; earpiece: %s\n",
		       getenv("XWIRED_AUDIO") ? getenv("XWIRED_AUDIO") : "tone (XWIRED_AUDIO=loop|silent)");
	} else if (mic_wav) {
		wav_header(mic_wav, gc->hs.rate_in ? gc->hs.rate_in : 16000, 1, mic_bytes);
		fclose(mic_wav);
		mic_wav = NULL;
	}
}

static void hs_audio_in(struct gip_client *gc, const u8 *samples, int len)
{
	if (mic_wav) {
		fwrite(samples, 1, len, mic_wav);
		mic_bytes += len;
	}
	for (int i = 0; i + 1 < len; i += 2) {
		loop_ring[loop_w % ARRAY_SIZE(loop_ring)] = (s16)(samples[i] | samples[i + 1] << 8);
		loop_w++;
	}
}

/* one 8 ms earpiece block: a 440 Hz tone, the mic (loop), or silence */
static void hs_fill_block(struct gip_client *gc, u8 *out)
{
	static const s16 sine[32] = { 0, 1595, 3129, 4545, 5793, 6827, 7608, 8105, 8276, 8105,
				      7608, 6827, 5793, 4545, 3129, 1595, 0, -1595, -3129, -4545,
				      -5793, -6827, -7608, -8105, -8276, -8105, -7608, -6827,
				      -5793, -4545, -3129, -1595 };
	static u32 phase;
	const char *mode = getenv("XWIRED_AUDIO");
	u32 ch = gc->hs.channels_out, n = gc->hs.buffer_out / (2 * ch);

	if (!loop_r && loop_w > 2000)
		loop_r = loop_w - 400; /* start the loopback a little behind */
	for (u32 i = 0; i < n; i++) {
		s16 v = 0;

		if (!mode || !strcmp(mode, "tone")) {
			v = sine[(phase >> 16) & 31];
			phase += (u32)((440ull * 32 << 16) / gc->hs.rate_out);
		} else if (!strcmp(mode, "loop") && loop_r && loop_r < loop_w) {
			/* nearest sample at the mic's rate */
			v = loop_ring[loop_r % ARRAY_SIZE(loop_ring)];
			if ((u64)i * gc->hs.rate_in / gc->hs.rate_out !=
			    (u64)(i + 1) * gc->hs.rate_in / gc->hs.rate_out)
				loop_r++;
		}
		for (u32 c = 0; c < ch; c++) {
			out[(i * ch + c) * 2] = (u8)v;
			out[(i * ch + c) * 2 + 1] = (u8)(v >> 8);
		}
	}
}

static void pad_ready(struct gip_client *gc)
{
	xa_log("pad: ready, press buttons (Ctrl-C quits)\n");
}

static void pad_input(struct gip_client *gc, const struct gip_gamepad_state *s)
{
	static const struct { u16 bit; const char *name; } names[] = {
		{ GIP_GP_BTN_A, "A" }, { GIP_GP_BTN_B, "B" }, { GIP_GP_BTN_X, "X" },
		{ GIP_GP_BTN_Y, "Y" }, { GIP_GP_BTN_BUMPER_L, "LB" }, { GIP_GP_BTN_BUMPER_R, "RB" },
		{ GIP_GP_BTN_VIEW, "View" }, { GIP_GP_BTN_MENU, "Menu" },
		{ GIP_GP_BTN_STICK_L, "LS" }, { GIP_GP_BTN_STICK_R, "RS" },
		{ GIP_GP_BTN_DPAD_U, "Up" }, { GIP_GP_BTN_DPAD_D, "Down" },
		{ GIP_GP_BTN_DPAD_L, "Left" }, { GIP_GP_BTN_DPAD_R, "Right" },
	};
	static struct gip_gamepad_state last;
	char line[160] = "";

	gip_set_rumble(gc, s->buttons & GIP_GP_BTN_A ? 60 : 0, s->buttons & GIP_GP_BTN_B ? 60 : 0,
		       s->trigger_left * 100 / 1023, s->trigger_right * 100 / 1023);

#define MOVED(a, b, t) (abs((int)(a) - (int)(b)) > (t))
	if (s->buttons == last.buttons && s->guide == last.guide &&
	    !MOVED(s->trigger_left, last.trigger_left, 100) &&
	    !MOVED(s->trigger_right, last.trigger_right, 100) &&
	    !MOVED(s->stick_left_x, last.stick_left_x, 6000) &&
	    !MOVED(s->stick_left_y, last.stick_left_y, 6000) &&
	    !MOVED(s->stick_right_x, last.stick_right_x, 6000) &&
	    !MOVED(s->stick_right_y, last.stick_right_y, 6000))
		return;
	last = *s;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (s->buttons & names[i].bit)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s ", names[i].name);
	if (s->guide)
		strcat(line, "Guide ");
	if (!*line)
		strcat(line, "(no buttons) ");
	xa_log("pad: %s LT %4u RT %4u  L %6d,%6d  R %6d,%6d\n", line, s->trigger_left,
	       s->trigger_right, s->stick_left_x, s->stick_left_y, s->stick_right_x,
	       s->stick_right_y);
}

static void pad_gone(struct gip_client *gc)
{
	xa_log("pad: off\n");
}

static const struct gip_ops pad_ops = {
	.ready = pad_ready,
	.input = pad_input,
	.gone = pad_gone,
	.headset = hs_changed,
	.audio_in = hs_audio_in,
};

/* the first device with a GIP interface 0, and that interface's interrupt endpoints */
static libusb_device *find_pad(u8 *in, u8 *out)
{
	libusb_device **list, *found = NULL;
	ssize_t n = libusb_get_device_list(NULL, &list);

	for (ssize_t i = 0; i < n && !found; i++) {
		struct libusb_device_descriptor dd;
		struct libusb_config_descriptor *cfg;

		/* the first configuration, not the active one: with no driver bound (macOS)
		 * the device isn't configured yet, and main() configures it */
		if (libusb_get_device_descriptor(list[i], &dd) ||
		    libusb_get_config_descriptor(list[i], 0, &cfg))
			continue;
		for (int f = 0; f < cfg->bNumInterfaces && !found; f++) {
			const struct libusb_interface_descriptor *id = &cfg->interface[f].altsetting[0];

			if (id->bInterfaceNumber || id->bInterfaceClass != GIP_CLASS ||
			    id->bInterfaceSubClass != GIP_SUBCLASS ||
			    id->bInterfaceProtocol != GIP_PROTOCOL)
				continue;
			*in = *out = 0;
			for (int e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];

				if ((ed->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_INTERRUPT)
					continue;
				if (ed->bEndpointAddress & 0x80)
					*in = ed->bEndpointAddress;
				else
					*out = ed->bEndpointAddress;
				xa_log("usb: endpoint %02x interrupt, %u bytes, interval %u\n",
				       ed->bEndpointAddress, ed->wMaxPacketSize, ed->bInterval);
			}
			xa_log("usb: GIP controller %04x:%04x (bus %u, port %u)\n", dd.idVendor,
			       dd.idProduct, libusb_get_bus_number(list[i]),
			       libusb_get_port_number(list[i]));
			if (*in && *out)
				found = libusb_ref_device(list[i]);
			else
				xa_log("usb: but it has no interrupt IN/OUT pair\n");
		}
		libusb_free_config_descriptor(cfg);
	}
	libusb_free_device_list(list, 1);
	return found;
}

static void on_signal(int sig)
{
	quit = 1;
}

int main(void)
{
	const char *dbg = getenv("XA_DEBUG");
	libusb_device *dev;
	struct gip_client *gc;
	int ret;

	xa_debug = dbg ? atoi(dbg) : 0;
	log_fd = open("xwired.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	transport.no_auth = !getenv("XWIRED_AUTH");
	audio_blocked = getenv("XWIRED_NOAUDIO") != NULL;
	xa_log("xwired: security handshake %s, headset audio %s\n",
	       transport.no_auth ? "off" : "on", audio_blocked ? "off (XWIRED_NOAUDIO)" : "on");
	signal(SIGINT, on_signal);

	if (libusb_init(NULL))
		return 1;
	dev = find_pad(&ep_in, &ep_out);
	if (!dev && getenv("XWIRED_WAIT")) {
		xa_log("usb: waiting for a wired GIP controller to be plugged in\n");
		while (!dev && !quit) {
			xa_sleep_ms(200);
			dev = find_pad(&ep_in, &ep_out);
		}
	}
	if (!dev) {
		xa_log("usb: no wired GIP controller found: plug the controller straight into "
		       "this computer with a data cable\n");
		return 1;
	}
	ret = libusb_open(dev, &handle);
	libusb_unref_device(dev);
	if (ret) {
		xa_log("usb: cannot open it: %s\n", libusb_error_name(ret));
		return 1;
	}
	libusb_set_auto_detach_kernel_driver(handle, 1);
	/* xone resets every wired controller first ("newer devices require a reset") */
	if (getenv("XWIRED_RESET")) { /* the device may re-enumerate: run again after it */
		ret = libusb_reset_device(handle);
		xa_log("usb: reset: %s\n", ret ? libusb_error_name(ret) : "ok");
	}
	{
		int cfg = 0;

		if (libusb_get_configuration(handle, &cfg) || cfg != 1) {
			ret = libusb_set_configuration(handle, 1);
			xa_log("usb: set configuration 1: %s\n", ret ? libusb_error_name(ret) : "ok");
		}
	}
	ret = libusb_claim_interface(handle, 0);
	if (ret) {
		xa_log("usb: cannot claim interface 0: %s (another driver has it; try sudo)\n",
		       libusb_error_name(ret));
		return 1;
	}

	/* xone: "disable the audio interface, mandatory for certain third party devices" */
	{
		struct libusb_config_descriptor *cfg;

		if (!libusb_get_active_config_descriptor(libusb_get_device(handle), &cfg)) {
			for (int f = 0; f < cfg->bNumInterfaces; f++)
				for (int a = 0; a < cfg->interface[f].num_altsetting; a++) {
					const struct libusb_interface_descriptor *id =
						&cfg->interface[f].altsetting[a];

					if (id->bInterfaceNumber != AUDIO_IF || id->bAlternateSetting != 1)
						continue;
					for (int e = 0; e < id->bNumEndpoints; e++) {
						const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
						unsigned int maxp = ed->wMaxPacketSize & 0x7ff;

						if (ed->bEndpointAddress == AUDIO_EP_IN && maxp &&
						    maxp <= 1023)
							audio_in_maxp = maxp;
						if (ed->bEndpointAddress == AUDIO_EP_OUT)
							audio_out_maxp = maxp;
						xa_log("usb: audio alt 1 endpoint %02x type %u, %u bytes, "
						       "interval %u\n", ed->bEndpointAddress,
						       ed->bmAttributes & 3, maxp, ed->bInterval);
					}
				}
			libusb_free_config_descriptor(cfg);
		}
	}
	ret = libusb_claim_interface(handle, 1);
	if (!ret) {
		ret = libusb_set_interface_alt_setting(handle, 1, 0);
		xa_log("usb: audio interface 1 to alternate 0: %s\n", ret ? libusb_error_name(ret) : "ok");
	} else {
		xa_log("usb: audio interface 1 not claimed: %s\n", libusb_error_name(ret));
	}

	if (getenv("XWIRED_PROBE")) {
		/* xpad's init packets (xboxone_init_packets), raw, then a rumble pulse: does the
		 * controller react at all? */
		static const u8 probe[][14] = {
			{ 5, 0x05, 0x20, 0x00, 0x01, 0x00 },                   /* power on */
			{ 5, 0x05, 0x20, 0x00, 0x0f, 0x06 },                   /* Xbox One S power on */
			{ 4, 0x04, 0x20, 0x00, 0x00 },                         /* identify request */
			{ 7, 0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 },       /* LED on */
			{ 13, 0x09, 0x00, 0x00, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x40, 0x40, 0x64, 0x00, 0x00 },
		};
		u8 seq = 1;

		for (size_t i = 0; i < ARRAY_SIZE(probe); i++) {
			u8 pkt[16];

			memcpy(pkt, probe[i] + 1, probe[i][0]);
			pkt[2] = seq++;
			wired_send(NULL, pkt, probe[i][0]);
			xa_sleep_ms(20);
		}
		xa_log("probe: sent power on, identify, LED on, a 1 s rumble\n");
	}

	gc = gip_client_new_transport(&transport, NULL, 0, &pad_ops, NULL);
	xa_log("gip: waiting for the controller to announce itself%s\n",
	       transport.no_auth ? "" : " (with the security handshake)");
	u64 next_stats = xa_now_ms() + 2000;
	bool unplugged = false;

	ret = int_in_start();
	if (ret) {
		xa_log("usb: cannot start reading the controller: %s\n", libusb_error_name(ret));
		quit = 1;
	}
	while (!quit) {
		if (xa_debug && xa_now_ms() > next_stats) {
			xa_log("usb: in: %u packets, %u errors, %u dropped\n", int_in_packets,
			       int_in_errors, int_in_dropped);
			next_stats = xa_now_ms() + 2000;
		}

		/* short: the earpiece needs a block every 8 ms (all callbacks run in here) */
		{
			struct timeval tv = { 0, (gc->hs.state == GIP_HS_READY ? 4 : 50) * 1000 };

			libusb_handle_events_timeout_completed(NULL, &tv, NULL);
		}
		while (inq_r != inq_w) {
			unsigned int i = inq_r % RXQ_LEN;

			if (xa_debug > 2)
				dump("in ", inq[i].data, inq[i].len);
			gip_rx(gc, inq[i].data, inq[i].len);
			inq_r++;
		}
		if (int_in_gone) {
			xa_log("usb: controller unplugged\n");
			unplugged = true;
			break;
		}
		audio_rx_drain(gc);
		gip_poll(gc);
		if (gc->hs.state == GIP_HS_READY) {
			static u64 next_block;
			static u8 block[4 * 48 * 2 * 8];
			static u64 next_report;
			u64 now = xa_now_ms();

			if (!next_block || now > next_block + 100)
				next_block = now;
			while (now >= next_block && gc->hs.buffer_out <= sizeof(block)) {
				hs_fill_block(gc, block);
				gip_headset_send(gc, block);
				next_block += 8;
			}
			if (xa_debug && now > next_report) {
				xa_log("headset: in %u packets (%u bytes recorded, %u errors, %u dropped), "
				       "out %u blocks (%u errors, %d in flight)\n", gc->hs.packets_in,
				       mic_bytes, audio_in_errors, audio_in_dropped, gc->hs.packets_out,
				       audio_out_errors, audio_out_inflight);
				next_report = now + 2000;
			}
		}
	}

	xa_log("xwired: quitting\n");
	if (gc->hs.state == GIP_HS_READY)
		hs_changed(gc, false);
	/* stop the iso streams and wait for them before letting go of the interfaces */
	if (wired_enable_audio(NULL, false)) {
		xa_log("usb: audio would not stop: not releasing interface 1 (unplug the "
		       "controller before running again)\n");
		gip_client_free(gc);
		return 1; /* exiting with transfers pending is safer than release under them */
	}
	for (int i = 0; i < AUDIO_IN_XFERS; i++)
		if (audio_in[i])
			libusb_free_transfer(audio_in[i]);
	if (!unplugged) {
		gip_set_rumble(gc, 0, 0, 0, 0);
		gip_poll(gc);
	}
	int_in_stop();
	gip_client_free(gc);
	libusb_release_interface(handle, 1);
	libusb_release_interface(handle, 0);
	libusb_close(handle);
	libusb_exit(NULL);
	return 0;
}
