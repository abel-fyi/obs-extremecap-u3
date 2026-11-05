/* AVerMedia ExtremeCap U3 (CV710, USB 07ca:0710) capture core.
 *
 * cv710_start() runs a background thread that finds the card, sets it up,
 * waits for HDMI, streams, and recovers by itself from signal loss, mode
 * changes and USB unplugs. Callbacks run on that thread.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum cv710_state { CV710_NO_DEVICE, CV710_NO_SIGNAL, CV710_UNSUPPORTED, CV710_LIVE };

struct cv710_mode {
	int width, height;
	bool interlaced;
	double fps;
};

struct cv710_callbacks {
	void *ctx;
	/* One complete YUY2 frame, width * 2 bytes per line. */
	void (*video)(void *ctx, const uint8_t *yuy2, int width, int height);
	/* Interleaved stereo 16-bit samples in host byte order. */
	void (*audio)(void *ctx, const int16_t *pcm, int frames, int rate);
	void (*state)(void *ctx, enum cv710_state state, const struct cv710_mode *mode);
	void (*log)(void *ctx, const char *message);
};

struct cv710 *cv710_start(const struct cv710_callbacks *callbacks);
void cv710_stop(struct cv710 *cv710);
