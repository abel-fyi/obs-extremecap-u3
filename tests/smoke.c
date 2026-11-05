/* Hardware smoke test for the core: run N seconds, report frames/audio/states.
 * Build: make smoke   Run: build/smoke 10 */
#include "../src/cv710.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static long frames, audio_frames, last_rate;
static int width, height;

static void video(void *ctx, const uint8_t *yuy2, int w, int h)
{
	(void)ctx, (void)yuy2;
	frames++, width = w, height = h;
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
	return frames > 0 ? 0 : 1;
}
