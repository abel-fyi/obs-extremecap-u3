/* CV710 capture core. See PROTOCOL.md for how the card works. */
#include "cv710.h"

#include <libusb.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 7-bit I2C addresses. The receiver maps are only reachable out of reset. */
enum { IO = 0x20, CP = 0x22, AFE = 0x26, REP = 0x32, HDMI = 0x34, GPIO = 0x74, VX = 0x18, LFE = 0x10, TI = 0x18 };
enum { MAX_W = 1920, MAX_H = 1080, TRANSFERS = 32, TRANSFER_SIZE = 65536, MAX_AUDIO = 16384 };

struct cv710 {
	struct cv710_callbacks cb;
	pthread_t thread;
	atomic_bool stop;
	libusb_context *usb;
	libusb_device_handle *dev;
	bool ok;                  /* sticky: false after any USB failure */
	int released;             /* receiver reset state: -1 unknown, 0 held, 1 running */
	uint8_t lfe_saved[9];
	/* status LED */
	atomic_bool recording;
	enum cv710_state state;
	int led_shown;
	/* stream parser */
	enum { SEEK, PAYLOAD, TAIL, AUDIO } ps;
	uint64_t window;          /* last 8 bytes seen while seeking */
	int seen;
	uint8_t *frame;
	size_t frame_size, fill;
	uint8_t head[4], tail[9], audio[MAX_AUDIO];
	int tail_fill;
	size_t audio_size, audio_fill;
	struct cv710_mode mode;
	/* audio rate measurement */
	uint64_t rate_start_ns, rate_frames;
	int rate;
	/* async capture */
	bool running, stalled;
	int active;
};

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void sleep_ms(int ms)
{
	struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
	nanosleep(&t, NULL);
}

static void logf_(struct cv710 *c, const char *fmt, ...)
{
	char text[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	if (c->cb.log)
		c->cb.log(c->cb.ctx, text);
}

static void set_state(struct cv710 *c, enum cv710_state state, const struct cv710_mode *mode)
{
	c->state = state;
	if (c->cb.state)
		c->cb.state(c->cb.ctx, state, mode);
}

/* ---- EP1 command channel ------------------------------------------------ */

static void fail(struct cv710 *c, const char *what, int error)
{
	if (c->ok)
		logf_(c, "USB %s failed: %s", what, libusb_error_name(error));
	c->ok = false;
}

static bool bulk(struct cv710 *c, unsigned char endpoint, uint8_t *data, int length)
{
	int done = 0, error;
	if (!c->ok)
		return false;
	error = libusb_bulk_transfer(c->dev, endpoint, data, length, &done, 1000);
	if (error || done != length) {
		fail(c, endpoint & 0x80 ? "read" : "write", error ? error : LIBUSB_ERROR_IO);
		return false;
	}
	return true;
}

/* I2C read (out != NULL) or write via the card's firmware, in <=32-byte pieces:
 * one oversized command wedged the firmware until a physical replug. */
static void i2c(struct cv710 *c, int addr, int reg, const uint8_t *data, uint8_t *out, int len, bool gpio_bus)
{
	bool receiver = addr == IO || addr == CP || addr == AFE || addr == REP || addr == HDMI;
	if (receiver && !gpio_bus && c->released != 1) {
		/* Reading receiver maps while it is held in reset stalls the firmware. */
		if (c->released == -1) {
			uint8_t pca;
			i2c(c, GPIO, 2, NULL, &pca, 1, false);
		}
		if (c->released != 1) {
			if (c->ok)
				logf_(c, "refusing receiver access while it is held in reset");
			c->ok = false;
			return;
		}
	}
	for (int off = 0; off < len && c->ok; off += 32) {
		int n = len - off < 32 ? len - off : 32;
		uint8_t packet[4 + 32];
		if (out) {
			uint8_t read_cmd[5] = {gpio_bus ? 4 : 2, (uint8_t)addr, 1, (uint8_t)n, (uint8_t)(reg + off)};
			if (bulk(c, 0x01, read_cmd, 5))
				bulk(c, 0x81, out + off, n);
		} else {
			packet[0] = gpio_bus ? 3 : 1;
			packet[1] = (uint8_t)addr;
			packet[2] = 1;
			packet[3] = (uint8_t)(reg + off);
			memcpy(packet + 4, data + off, (size_t)n);
			bulk(c, 0x01, packet, 4 + n);
		}
	}
	if (c->ok && addr == GPIO && !gpio_bus && reg <= 2 && 2 < reg + len)
		c->released = ((out ? out : data)[2 - reg] & 0x40) != 0;
}

static uint8_t rd(struct cv710 *c, int addr, int reg)
{
	uint8_t v = 0;
	i2c(c, addr, reg, NULL, &v, 1, false);
	return v;
}

static void wr(struct cv710 *c, int addr, int reg, uint8_t v)
{
	i2c(c, addr, reg, &v, NULL, 1, false);
}

static void upd(struct cv710 *c, int addr, int reg, uint8_t mask, uint8_t bits)
{
	uint8_t v = rd(c, addr, reg);
	wr(c, addr, reg, (uint8_t)((v & ~mask) | bits));
}

static void gpio(struct cv710 *c, int bit, int value)
{
	upd(c, GPIO, 2, (uint8_t)(1 << bit), (uint8_t)(value << bit)); /* output level */
	upd(c, GPIO, 6, (uint8_t)(1 << bit), 0);                       /* direction: output */
}

/* ---- HDMI receiver setup (ADV7604-class receiver, board-specific) --------- */

struct pair { uint8_t reg, value; };

static void receiver_init(struct cv710 *c)
{
	static const struct pair ti[] = {{3, 0x90}, {4, 0x74}, {5, 0x13}, {6, 0xdc}, {0x0b, 1}, {8, 0xc0},
		{9, 0x30}, {0x0c, 0x50}, {0x15, 0x78}, {0x18, 0x78}, {0x0f, 0x0d}, {0x16, 0x1c},
		{0x10, 0x0d}, {0x13, 0x1c}, {0x0f, 0x0d}, {0x10, 0x0d}};
	static const struct pair maps[] = {{0xf4, 0x80}, {0xf5, 0x7c}, {0xf6, 0x70}, {0xf7, 0x78},
		{0xf8, 0x4c}, {0xf9, 0x64}, {0xfa, 0x6c}, {0xfb, 0x68}, {0xfc, 0x52}, {0xfd, 0x44},
		{0xfe, 0x48}, {0xf3, 0x84}};
	static const struct pair hdmi[] = {{0x0d, 0x84}, {0x3d, 0x10}, {0x3e, 0x39}, {0x4e, 0x3b},
		{0x57, 0xb6}, {0x58, 3}, {0x9d, 1}, {0x8d, 0x18}, {0x8e, 0x34}, {0x93, 0x8b},
		{0x94, 0x2d}, {0x96, 1}, {0x48, 6}, {0x14, 0x1d}, {0x1a, 0x40}};
	static const struct pair afe[] = {{1, 0x2e}, {0x0c, 0x1f}, {0x12, 0x7b}, {0xc8, 0x40}};
	static const struct pair cp[] = {{0x7c, 0xc0}, {0x7d, 0}, {0x8b, 0x40}, {0x8c, 0},
		{0x8d, 0}, {0x8e, 0}, {0xbe, 0}, {0xbf, 0x12}};
	/* CP two-byte fields cleared under a mask (last one sets 0x40). */
	static const struct { uint8_t reg, mask[2], value[2]; } blocks[] = {
		{0x8f, {0x07, 0xff}, {0, 0}}, {0x47, {0x07, 0xff}, {0, 0}}, {0xab, {0xff, 0xf0}, {0, 0}},
		{0x46, {0xff, 0xe0}, {0, 0}}, {0xa2, {0xff, 0xf0}, {0, 0}}, {0xa3, {0x0f, 0xff}, {0, 0}},
		{0xa5, {0xff, 0xf0}, {0, 0}}, {0xa6, {0x0f, 0xff}, {0, 0}}, {0xa8, {0xff, 0xf0}, {0, 0}},
		{0xa9, {0x0f, 0xff}, {0, 0}}, {0x8a, {0x01, 0xf0}, {0x00, 0x40}}};
	static const int routing[][2] = {{0, 1}, {1, 1}, {0, 1}, {1, 0}};

	/* Board: source routing, HPD deasserted (GPIO2 is inverted), reset pulses. */
	for (size_t i = 0; i < 4; i++)
		gpio(c, routing[i][0], routing[i][1]);
	gpio(c, 2, 1);
	for (int bit = 6; bit >= 5; bit--)
		for (int v = 1, n = 0; n < 3; n++, v = !v) {
			gpio(c, bit, v);
			sleep_ms(1);
		}
	/* Audio clocks (TI codec on the separate GPIO I2C bus) are needed for streaming. */
	for (size_t i = 0; i < sizeof(ti) / sizeof(*ti); i++)
		i2c(c, TI, ti[i].reg, &ti[i].value, NULL, 1, true);

	for (size_t i = 0; i < sizeof(maps) / sizeof(*maps); i++)
		upd(c, IO, maps[i].reg, 0xfe, maps[i].value);
	uint8_t revision = rd(c, IO, 0x11);
	if (c->ok && revision != 3) {
		logf_(c, "unknown receiver revision %#x", revision);
		c->ok = false;
	}
	upd(c, HDMI, 0, 3, 0);       /* port A */
	upd(c, HDMI, 1, 0xf8, 0x70); /* port A termination, automatic power */
	upd(c, IO, 0x0c, 0x22, 2);   /* receiver powered */
	upd(c, IO, 0x0b, 4, 4);      /* analog front end off */
	upd(c, IO, 1, 0xff, 5);      /* HDMI component mode */
	upd(c, IO, 0, 0x3f, 0x1e);
	upd(c, IO, 0x15, 0x1e, 0);   /* output pins on */
	for (size_t i = 0; i < sizeof(hdmi) / sizeof(*hdmi); i++)
		wr(c, HDMI, hdmi[i].reg, hdmi[i].value);
	wr(c, CP, 0xcf, 1);
	for (size_t i = 0; i < sizeof(afe) / sizeof(*afe); i++)
		wr(c, AFE, afe[i].reg, afe[i].value);
	upd(c, AFE, 0xc6, 0x20, 0x20); /* audio clock out */
	upd(c, HDMI, 1, 3, 2);         /* I2S audio */
	upd(c, HDMI, 2, 3, 2);
	upd(c, HDMI, 3, 0x60, 0);
	/* The board's own EDID EEPROM shares the DDC lines; the receiver's internal
	 * EDID must stay off or sources read both at once (corrupt checksum). */
	upd(c, REP, 0x77, 0x0f, 0);

	/* Output: auto colour conversion, limited-range YCbCr 4:2:2, board pinout. */
	upd(c, CP, 0x69, 0x38, 0x30);
	upd(c, CP, 0x68, 0xf0, 0xf0);
	wr(c, IO, 2, 0xf5);
	wr(c, IO, 3, 0x80);
	upd(c, IO, 4, 0xe0, 0x40);
	upd(c, IO, 0x30, 0x10, 0);
	upd(c, IO, 5, 0x11, 0);
	upd(c, IO, 6, 0xe6, 0xa6);
	upd(c, IO, 0xdd, 0xf0, 0);
	upd(c, IO, 7, 0x7f, 0x40);
	upd(c, IO, 8, 0x3f, 0x3f);
	upd(c, CP, 0x85, 0x18, 0);
	upd(c, CP, 0x41, 0x18, 8);
	upd(c, IO, 0x14, 0x3f, 0x3f);
	upd(c, IO, 0x16, 0x80, 0);
	upd(c, CP, 0x91, 0x40, 0x40);
	for (size_t i = 0; i < sizeof(blocks) / sizeof(*blocks); i++) {
		uint8_t v[2];
		i2c(c, CP, blocks[i].reg, NULL, v, 2, false);
		for (int k = 0; k < 2; k++)
			v[k] = (uint8_t)((v[k] & ~blocks[i].mask[k]) | blocks[i].value[k]);
		i2c(c, CP, blocks[i].reg, v, NULL, 2, false);
	}
	upd(c, CP, 0x7b, 4, 4);
	upd(c, CP, 0x8a, 0xf8, 0x20);
	upd(c, AFE, 0x15, 0x0c, 4);
	for (size_t i = 0; i < sizeof(cp) / sizeof(*cp); i++)
		wr(c, CP, cp[i].reg, cp[i].value);
	sleep_ms(150);
	gpio(c, 2, 0); /* assert hotplug: sources may now read the EDID */
}

/* Bypass the VX scaler chip (reset, direct output mode, direct video path).
 * Returns its post-reset registers 0x8a/0x0d for restoring later. */
static void scaler_bypass(struct cv710 *c, uint8_t saved[2])
{
	for (int v = 1, n = 0; n < 3; n++, v = !v) {
		gpio(c, 4, v);
		sleep_ms(1);
	}
	saved[0] = rd(c, VX, 0x8a);
	saved[1] = rd(c, VX, 0x0d);
	wr(c, VX, 0x8a, saved[0] | 2);
	wr(c, VX, 0x0d, saved[1] | 0x80);
	gpio(c, 3, 0);
	sleep_ms(100);
}

/* Measured input, or false when there is no locked HDMI signal. */
static bool timing(struct cv710 *c, struct cv710_mode *m)
{
	uint8_t h[12], w[2];
	uint8_t source = rd(c, IO, 0x6f), locks = rd(c, IO, 0x6a);
	i2c(c, HDMI, 0, NULL, h, 12, false);
	if (!c->ok || !(source & 0x10) || (locks & 0x70) != 0x70 || !(h[4] & 2) || (h[0] & 3))
		return false;
	m->width = ((h[7] & 15) << 8) | h[8];
	m->height = ((h[9] & 15) << 8) | h[10];
	m->interlaced = h[11] & 0x10;
	i2c(c, HDMI, 0x1e, NULL, w, 2, false);
	double htotal = ((w[0] << 8) | w[1]) & 0x0fff;
	i2c(c, HDMI, 0x26, NULL, w, 2, false);
	double vtotal = (((w[0] << 8) | w[1]) & 0x1fff) / 2.0;
	double tmds = h[6] * 1e6 + ((rd(c, HDMI, 0x3b) >> 4) & 3) * 250e3;
	double pixel = tmds * 8 / (8 + ((h[11] & 0x60) >> 4)) / ((h[5] & 15) + 1);
	m->fps = htotal && vtotal ? pixel / (htotal * vtotal) : 0;
	return c->ok;
}

static bool same_mode(const struct cv710_mode *a, const struct cv710_mode *b)
{
	return a->width == b->width && a->height == b->height && a->interlaced == b->interlaced &&
	       fabs(a->fps - b->fps) < 0.1;
}

/* Progressive modes the card delivers as native-size YUY2 (tested 640x480..1080p). */
static bool supported(const struct cv710_mode *m)
{
	return !m->interlaced && m->width > 0 && m->width % 2 == 0 && m->width <= MAX_W &&
	       m->height > 0 && m->height <= MAX_H && m->fps >= 23 && m->fps <= 76;
}

/* ---- Stream parser ----------------------------------------------------------
 * 00ffffc0 seq meta flags sum | width*height*2 YUY2 | 00ffffc1 seq m1 m2 m3 sum
 * 00ffff58 u32be length | big-endian int16 stereo PCM   (between frames)
 */
static void audio_record(struct cv710 *c)
{
	int frames = (int)(c->audio_size / 4);
	int16_t *pcm = (int16_t *)c->audio;
	for (int i = 0; i < frames * 2; i++) {
		uint8_t *b = c->audio + i * 2;
		pcm[i] = (int16_t)((b[0] << 8) | b[1]);
	}
	/* HDMI allows 32/44.1/48 kHz; measure which one over >= 1 s of records.
	 * Without source audio the card sends zero-filled records at a rate that
	 * matches none of them: those are dropped (rate 0). */
	uint64_t now = now_ns();
	if (!c->rate_start_ns) {
		c->rate_start_ns = now;
		c->rate_frames = 0;
	} else if (now - c->rate_start_ns >= 1000000000u) {
		double measured = c->rate_frames * 1e9 / (double)(now - c->rate_start_ns);
		static const int rates[] = {32000, 44100, 48000};
		c->rate = 0;
		for (int i = 0; i < 3; i++)
			if (fabs(measured - rates[i]) < rates[i] * 0.03)
				c->rate = rates[i];
		c->rate_start_ns = now;
		c->rate_frames = 0;
	}
	c->rate_frames += (uint64_t)frames;
	if (c->rate && c->cb.audio)
		c->cb.audio(c->cb.ctx, pcm, frames, c->rate);
}

static void parse(struct cv710 *c, const uint8_t *p, size_t n)
{
	while (n) {
		if (c->ps == PAYLOAD || c->ps == AUDIO) {
			uint8_t *dst = c->ps == PAYLOAD ? c->frame : c->audio;
			size_t want = (c->ps == PAYLOAD ? c->frame_size : c->audio_size) - c->fill;
			size_t take = n < want ? n : want;
			memcpy(dst + c->fill, p, take);
			c->fill += take, p += take, n -= take;
			if (c->fill < (c->ps == PAYLOAD ? c->frame_size : c->audio_size))
				return;
			if (c->ps == AUDIO)
				audio_record(c);
			c->ps = c->ps == PAYLOAD ? TAIL : SEEK;
			c->tail_fill = 0, c->seen = 0;
			continue;
		}
		if (c->ps == TAIL) {
			c->tail[c->tail_fill++] = *p++, n--;
			if (c->tail_fill < 9)
				continue;
			const uint8_t *t = c->tail;
			if (t[0] == 0 && t[1] == 0xff && t[2] == 0xff && t[3] == 0xc1 && t[4] == c->head[0] &&
			    t[8] == (uint8_t)(0xc1 + t[4] + t[5] + t[6] + t[7]) && c->cb.video)
				c->cb.video(c->cb.ctx, c->frame, c->mode.width, c->mode.height);
			c->ps = SEEK, c->seen = 0;
			continue;
		}
		/* SEEK: slide an 8-byte window looking for a checked frame or audio header. */
		c->window = (c->window << 8) | *p++, n--;
		if (++c->seen < 8)
			continue;
		uint32_t marker = (uint32_t)(c->window >> 32), arg = (uint32_t)c->window;
		if (marker == 0x00ffffc0) {
			uint8_t h[4] = {arg >> 24, arg >> 16, arg >> 8, arg};
			if (h[3] == (uint8_t)(0xc0 + h[0] + h[1] + h[2])) {
				memcpy(c->head, h, 4);
				c->ps = PAYLOAD, c->fill = 0;
			}
		} else if (marker == 0x00ffff58 && arg && arg <= MAX_AUDIO && arg % 4 == 0) {
			c->ps = AUDIO, c->audio_size = arg, c->fill = 0;
		}
	}
}

/* ---- Streaming -------------------------------------------------------------- */

static void LIBUSB_CALL transfer_done(struct libusb_transfer *t)
{
	struct cv710 *c = t->user_data;
	if (t->actual_length > 0)
		parse(c, t->buffer, (size_t)t->actual_length);
	if (t->status == LIBUSB_TRANSFER_COMPLETED && c->running &&
	    libusb_submit_transfer(t) == 0)
		return;
	if (t->status == LIBUSB_TRANSFER_TIMED_OUT)
		c->stalled = true; /* no data for 1 s: usually the HDMI signal went away */
	else if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED)
		c->ok = false;     /* e.g. the card was unplugged */
	c->running = false;
	c->active--;
}

static void lfe_write(struct cv710 *c, int reg, uint8_t v)
{
	uint8_t cmd[5] = {1, LFE, 1, (uint8_t)reg, v};
	bulk(c, 0x01, cmd, 5);
}

/* ---- Status LED --------------------------------------------------------------
 * FPGA registers 0x80-0x86 program up to four steps: 0x80 = colour << 4 | style
 * (1 steady, 2 blink, 3 fade), 0x81 brightness in %, 0x82 fade start in %,
 * 0x83/0x84 step length in ms, 0x85 fade (2), 0x86 = steps << 4 | step. Colours: 1 red, 2 blue, 3 both (pink).
 * Steady = all good; blinking = needs attention; red = OBS is recording. */
enum { RED = 1, BLUE = 2, PINK = 3 };

static void led(struct cv710 *c, int colour, bool blink)
{
	int code = colour << 1 | blink;
	if (code == c->led_shown)
		return;
	c->led_shown = code;
	uint8_t style = (uint8_t)(colour << 4 | (blink ? 2 : 1));
	if (!blink) {
		lfe_write(c, 0x80, style);
		lfe_write(c, 0x81, 100);
		lfe_write(c, 0x86, 0x11);
		return;
	}
	for (int step = 1; step <= 2; step++) { /* 200 ms off, 200 ms on */
		lfe_write(c, 0x80, style);
		lfe_write(c, 0x81, step == 1 ? 0 : 100);
		lfe_write(c, 0x83, 0);
		lfe_write(c, 0x84, 200);
		lfe_write(c, 0x86, (uint8_t)(0x20 | step));
	}
}

/* When we let go of the card: blue breathing (1 s fade in, 1 s fade out). */
static void led_idle(struct cv710 *c)
{
	for (int step = 1; step <= 2; step++) {
		lfe_write(c, 0x80, BLUE << 4 | 3); /* style 3: fade */
		lfe_write(c, 0x81, step == 1 ? 0 : 100);
		lfe_write(c, 0x82, step == 1 ? 100 : 0);
		lfe_write(c, 0x83, 0x03);
		lfe_write(c, 0x84, 0xe8);
		lfe_write(c, 0x85, 2);
		lfe_write(c, 0x86, (uint8_t)(0x20 | step));
	}
}

static void led_update(struct cv710 *c)
{
	int colour = atomic_load(&c->recording) ? RED : BLUE;
	if (c->state == CV710_LIVE)
		led(c, colour, false);
	else if (c->state == CV710_UNSUPPORTED)
		led(c, PINK, true);
	else
		led(c, colour, true);
}

/* Stream the current mode until the signal changes, stalls, or we are stopped. */
static void stream(struct cv710 *c)
{
	struct libusb_transfer *t[TRANSFERS] = {0};
	struct cv710_mode now;
	c->frame_size = (size_t)c->mode.width * c->mode.height * 2;
	c->ps = SEEK, c->seen = 0, c->rate_start_ns = 0, c->rate = 0;
	c->running = true, c->stalled = false, c->active = 0;

	/* FPGA: idle, YUY2, 1080 profile (other sizes arrive at native size anyway),
	 * 60 Hz profile, HDMI input; then reset the stream DMA. */
	lfe_write(c, 5, c->lfe_saved[5] & ~2);
	lfe_write(c, 6, 1);
	lfe_write(c, 1, 0x0c);
	lfe_write(c, 2, 0);
	lfe_write(c, 8, 1);
	uint8_t dma_reset = 0x14;
	bulk(c, 0x01, &dma_reset, 1);
	for (int i = 0; i < TRANSFERS && c->ok; i++) {
		t[i] = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(t[i], c->dev, 0x83, malloc(TRANSFER_SIZE), TRANSFER_SIZE,
					  transfer_done, c, 1000);
		if (libusb_submit_transfer(t[i]) == 0)
			c->active++;
		else
			c->ok = false;
	}
	lfe_write(c, 5, c->lfe_saved[5] | 2); /* work */
	uint64_t checked = now_ns();
	while (c->ok && c->running && !atomic_load(&c->stop)) {
		struct timeval tv = {0, 50000};
		led_update(c);
		libusb_handle_events_timeout(c->usb, &tv);
		if (now_ns() - checked > 500000000u) {
			checked = now_ns();
			if (!timing(c, &now) || !same_mode(&now, &c->mode))
				break;
		}
	}
	c->running = false;
	bool ok = c->ok;
	c->ok = true; /* still try to idle the FPGA and drain transfers */
	lfe_write(c, 5, c->lfe_saved[5] & ~2);
	for (int i = 0; i < TRANSFERS; i++)
		if (t[i])
			libusb_cancel_transfer(t[i]);
	for (int spins = 0; c->active > 0 && spins < 100; spins++) {
		struct timeval tv = {0, 50000};
		libusb_handle_events_timeout(c->usb, &tv);
	}
	for (int i = 0; i < TRANSFERS; i++)
		if (t[i]) {
			free(t[i]->buffer);
			libusb_free_transfer(t[i]);
		}
	static const int restore[] = {6, 1, 2, 8, 5};
	for (int i = 0; i < 5; i++)
		lfe_write(c, restore[i], c->lfe_saved[restore[i]]);
	c->ok = ok && c->ok;
}

/* Wait until the measured input stays the same for one second. */
static bool wait_stable(struct cv710 *c, struct cv710_mode *m)
{
	struct cv710_mode prev = {0};
	uint64_t since = 0;
	while (c->ok && !atomic_load(&c->stop)) {
		struct cv710_mode cur;
		led_update(c);
		if (timing(c, &cur)) {
			if (!since || !same_mode(&cur, &prev))
				since = now_ns(), prev = cur;
			else if (now_ns() - since >= 1000000000u) {
				*m = cur;
				return true;
			}
		} else {
			since = 0;
		}
		sleep_ms(50);
	}
	return false;
}

/* One USB connection: set up, stream whatever arrives, restore on the way out. */
static void session(struct cv710 *c)
{
	static const int lfe_regs[] = {1, 2, 5, 6, 8}, pca_regs[] = {2, 3, 6, 7};
	uint8_t pca[4], vx[2];
	c->ok = true, c->released = -1;
	/* GPIO expander outputs and directions; single registers only (block reads wrap). */
	for (int i = 0; i < 4; i++)
		pca[i] = rd(c, GPIO, pca_regs[i]);
	for (int i = 0; i < 5; i++) {
		uint8_t cmd[5] = {2, LFE, 1, 1, (uint8_t)lfe_regs[i]};
		if (bulk(c, 0x01, cmd, 5))
			bulk(c, 0x81, &c->lfe_saved[lfe_regs[i]], 1);
	}
	c->led_shown = -1;
	receiver_init(c);
	scaler_bypass(c, vx);
	if (c->ok)
		logf_(c, "card ready");
	set_state(c, CV710_NO_SIGNAL, NULL);
	while (c->ok && !atomic_load(&c->stop)) {
		struct cv710_mode m;
		if (!wait_stable(c, &m))
			break;
		logf_(c, "input %dx%d%c %.2f Hz", m.width, m.height, m.interlaced ? 'i' : 'p', m.fps);
		c->mode = m;
		if (!supported(&m)) {
			struct cv710_mode cur;
			set_state(c, CV710_UNSUPPORTED, &m);
			while (c->ok && !atomic_load(&c->stop) && timing(c, &cur) && same_mode(&cur, &m)) {
				led_update(c);
				sleep_ms(250);
			}
			continue;
		}
		set_state(c, CV710_LIVE, &m);
		stream(c);
		if (c->ok && !atomic_load(&c->stop))
			set_state(c, CV710_NO_SIGNAL, NULL);
	}
	if (c->ok) { /* leave the board as we found it */
		wr(c, VX, 0x8a, vx[0]);
		wr(c, VX, 0x0d, vx[1]);
		for (int i = 0; i < 4; i++)
			wr(c, GPIO, pca_regs[i], pca[i]);
		led_idle(c);
	}
}

static void *run(void *arg)
{
	struct cv710 *c = arg;
	bool announced = false;
	while (!atomic_load(&c->stop)) {
		c->dev = libusb_open_device_with_vid_pid(c->usb, 0x07ca, 0x0710);
		if (c->dev && libusb_claim_interface(c->dev, 0) == 0) {
			announced = false;
			session(c);
			libusb_release_interface(c->dev, 0);
		} else if (!announced) {
			logf_(c, c->dev ? "card is busy (another program has it open)" : "card not connected");
			announced = true;
		}
		if (c->dev)
			libusb_close(c->dev);
		c->dev = NULL;
		if (!atomic_load(&c->stop)) {
			set_state(c, CV710_NO_DEVICE, NULL);
			sleep_ms(1000);
		}
	}
	return NULL;
}

struct cv710 *cv710_start(const struct cv710_callbacks *callbacks)
{
	struct cv710 *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->cb = *callbacks;
	c->frame = malloc((size_t)MAX_W * MAX_H * 2);
	if (!c->frame || libusb_init(&c->usb) != 0 || pthread_create(&c->thread, NULL, run, c) != 0) {
		if (c->usb)
			libusb_exit(c->usb);
		free(c->frame);
		free(c);
		return NULL;
	}
	return c;
}

void cv710_set_recording(struct cv710 *c, bool recording)
{
	if (c)
		atomic_store(&c->recording, recording);
}

void cv710_stop(struct cv710 *c)
{
	if (!c)
		return;
	atomic_store(&c->stop, true);
	pthread_join(c->thread, NULL);
	libusb_exit(c->usb);
	free(c->frame);
	free(c);
}
