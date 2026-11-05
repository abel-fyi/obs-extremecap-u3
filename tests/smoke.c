/* Hardware smoke test for the core: run N seconds, report frames/audio/states,
 * optionally save the last frame as a PPM image (BT.601 limited range).
 * Build: make smoke   Run: build/smoke 10 [frame.ppm] */
#include "../src/cv710.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long frames, audio_frames, last_rate;
static int width, height;
static uint8_t *last; /* copy of the newest frame, if saving one */

static void video(void *ctx, const uint8_t *yuy2, int w, int h)
{
	(void)ctx, (void)yuy2;
	frames++, width = w, height = h;
	if (last)
		memcpy(last, yuy2, (size_t)w * h * 2);
}

static uint8_t clamp(double v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5);
}

static void save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (int i = 0; i < width * height / 2; i++) {
		const uint8_t *p = last + i * 4; /* Y0 U Y1 V */
		double u = p[1] - 128, v = p[3] - 128;
		for (int k = 0; k < 2; k++) {
			double y = 1.164383 * (p[k * 2] - 16);
			uint8_t rgb[3] = {clamp(y + 1.596027 * v), clamp(y - 0.391762 * u - 0.812968 * v),
					  clamp(y + 2.017232 * u)};
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void audio(void *ctx, const int16_t *pcm, int n, int rate)
{
	(void)ctx, (void)pcm;
	audio_frames += n, last_rate = rate;
}

static void state(void *ctx, enum cv710_state s, const struct cv710_mode *m)
{
	static const char *names[] = {"no device", "no signal", "unsupported", "live"};
	(void)ctx;
	if (m)
		printf("state: %s %dx%d%c %.2f Hz\n", names[s], m->width, m->height,
		       m->interlaced ? 'i' : 'p', m->fps);
	else
		printf("state: %s\n", names[s]);
}

static void log_line(void *ctx, const char *message)
{
	(void)ctx;
	printf("cv710: %s\n", message);
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 10;
	struct cv710_callbacks cb = {NULL, video, audio, state, log_line};
	if (argc > 2)
		last = malloc(1920 * 1080 * 2);
	struct cv710 *c = cv710_start(&cb);
	if (!c)
		return 1;
	for (int i = 0; i < seconds; i++) {
		sleep(1);
		printf("t=%2ds frames=%ld (%dx%d) audio=%ld frames @ %ld Hz\n", i + 1, frames, width,
		       height, audio_frames, last_rate);
		fflush(stdout);
	}
	cv710_stop(c);
	if (last && frames)
		save_ppm(argv[2]);
	return frames > 0 ? 0 : 1;
}
