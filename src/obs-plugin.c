/* OBS source "AVerMedia ExtremeCap U3 (CV710)": video and audio from the core. */
#include <obs-module.h>
#include <util/platform.h>

#include "cv710.h"

OBS_DECLARE_MODULE()

struct source {
	obs_source_t *source;
	struct cv710 *card;
};

static void on_video(void *ctx, const uint8_t *yuy2, int width, int height)
{
	struct source *s = ctx;
	struct obs_source_frame frame = {
		.data = {(uint8_t *)yuy2},
		.linesize = {(uint32_t)width * 2},
		.width = (uint32_t)width,
		.height = (uint32_t)height,
		.format = VIDEO_FORMAT_YUY2,
		.timestamp = os_gettime_ns(),
	};
	/* The receiver outputs limited-range BT.601 YCbCr (see PROTOCOL.md). */
	video_format_get_parameters_for_format(VIDEO_CS_601, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_YUY2,
					       frame.color_matrix, frame.color_range_min,
					       frame.color_range_max);
	obs_source_output_video(s->source, &frame);
}

static void on_audio(void *ctx, const int16_t *pcm, int frames, int rate)
{
	struct source *s = ctx;
	struct obs_source_audio audio = {
		.data = {(const uint8_t *)pcm},
		.frames = (uint32_t)frames,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_16BIT,
		.samples_per_sec = (uint32_t)rate,
		/* the record ends now; its first sample is one record earlier */
		.timestamp = os_gettime_ns() - util_mul_div64((uint64_t)frames, 1000000000u, (uint64_t)rate),
	};
	obs_source_output_audio(s->source, &audio);
}

static void on_state(void *ctx, enum cv710_state state, const struct cv710_mode *mode)
{
	struct source *s = ctx;
	(void)mode;
	if (state != CV710_LIVE)
		obs_source_output_video(s->source, NULL); /* blank instead of a frozen frame */
}

static void on_log(void *ctx, const char *message)
{
	(void)ctx;
	blog(LOG_INFO, "[cv710] %s", message);
}

static const char *get_name(void *unused)
{
	(void)unused;
	return "AVerMedia ExtremeCap U3 (CV710)";
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	struct source *s = bzalloc(sizeof(*s));
	struct cv710_callbacks cb = {s, on_video, on_audio, on_state, on_log};
	s->source = source;
	obs_source_set_async_unbuffered(source, true); /* show frames as they arrive */
	s->card = cv710_start(&cb);
	if (!s->card)
		blog(LOG_ERROR, "[cv710] could not start (libusb unavailable?)");
	return s;
}

static void destroy(void *data)
{
	struct source *s = data;
	cv710_stop(s->card);
	bfree(s);
}

static struct obs_source_info cv710_source = {
	.id = "cv710_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
};

bool obs_module_load(void)
{
	obs_register_source(&cv710_source);
	return true;
}
