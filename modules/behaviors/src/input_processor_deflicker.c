/*
 * Deflicker input processor — suppresses initial movement jerk
 *
 * Trackballs have higher static friction than kinetic friction, so the
 * first sample(s) after an idle gap tend to carry an exaggerated delta
 * ("breakaway spike").  This processor applies two complementary
 * techniques during a short warmup window after each idle gap:
 *
 *   1. Ramp scaling — attenuates early samples with a progressive
 *      weight: step / (ramp_len + 1).
 *   2. Softening — blends each output with the previous output
 *      per axis: out = (scaled + prev) / 2.  Inspired by the X.Org
 *      pointer acceleration "softening" option, this is inherently
 *      adaptive: large spikes get averaged down, small movements
 *      pass through with minimal change.
 *
 * The combination produces a smooth curve instead of a staircase,
 * and eliminates the hard jump at the end of the ramp window.
 *
 * Effective attenuation for ramp_len=3 (ramp + softening combined):
 *   sample 1 → ~12%   (spike almost fully absorbed)
 *   sample 2 → ~31%
 *   sample 3 → ~53%
 *   sample 4 → 100%   (passthrough, zero overhead)
 *
 * param1 = ramp-up length in samples (1-8, default 3)
 *   1 = minimal  (one dampened sample, ~25% effective)
 *   3 = recommended  (~36 ms warmup at 12 ms report interval)
 *   5 = aggressive  (very gentle start, noticeable on fast flicks)
 *
 * Idle gap threshold: 50 ms (matches the smooth processor).
 */

#define DT_DRV_COMPAT zmk_input_processor_deflicker

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define IDLE_RESET_MS 50
#define MAX_RAMP 8

struct deflicker_data {
	int64_t last_event_time;
	int16_t prev_x;     /* previous output for softening */
	int16_t prev_y;
	uint8_t warmup;     /* samples remaining (0 = passthrough) */
	uint8_t warmup_len;
	bool primed;
};

static int deflicker_handle_event(const struct device *dev,
				  struct input_event *event,
				  uint32_t param1, uint32_t param2,
				  struct zmk_input_processor_state *state)
{
	struct deflicker_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}
	if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
		return 0;
	}

	uint8_t ramp_len = (uint8_t)CLAMP(param1, 1, MAX_RAMP);
	int64_t now = k_uptime_get();

	/* Detect idle gap → reset warmup */
	if (!data->primed || (now - data->last_event_time) > IDLE_RESET_MS) {
		data->warmup = ramp_len;
		data->warmup_len = ramp_len;
		data->prev_x = 0;
		data->prev_y = 0;
		data->primed = true;
	}
	data->last_event_time = now;

	/* Select per-axis softening state */
	int16_t *prev = (event->code == INPUT_REL_X)
			? &data->prev_x : &data->prev_y;

	if (data->warmup > 0) {
		/*
		 * Stage 1 — Ramp scaling
		 *
		 * weight = step / (ramp_len + 1)
		 * For ramp_len=3:  step 1→1/4, step 2→2/4, step 3→3/4
		 */
		uint8_t step = data->warmup_len - data->warmup + 1;
		uint8_t divisor = data->warmup_len + 1;
		int16_t scaled = (int16_t)(((int32_t)event->value * step)
					   / divisor);

		/*
		 * Stage 2 — Softening (X.Org style)
		 *
		 * Blend scaled value with previous output for this axis.
		 * This is adaptive: a large spike gets averaged with the
		 * small (or zero) previous output, further dampening it.
		 * A genuine movement builds up prev naturally.
		 */
		int16_t out = (int16_t)(((int32_t)scaled + *prev) / 2);
		*prev = out;
		event->value = out;

		/*
		 * Advance warmup on sync (last event of the report) so
		 * the ramp ticks once per sensor report, not per axis.
		 * Fixes: pure vertical movement no longer stalls the ramp.
		 */
		if (event->sync) {
			data->warmup--;
		}
	} else {
		/* Track output for future warmup seeding */
		*prev = (int16_t)event->value;
	}

	return 0;
}

static struct zmk_input_processor_driver_api deflicker_api = {
	.handle_event = deflicker_handle_event,
};

#define DEFLICKER_INST(n)                                              \
	static struct deflicker_data deflicker_data_##n;               \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                           \
			      &deflicker_data_##n, NULL,               \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &deflicker_api);

DT_INST_FOREACH_STATUS_OKAY(DEFLICKER_INST)
