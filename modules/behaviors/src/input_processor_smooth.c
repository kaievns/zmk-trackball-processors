/*
 * EMA smoothing input processor
 *
 * Applies exponential moving average to relative X/Y input events.
 * Smooths out sensor jitter while preserving intentional movement.
 *
 * Smoothing level (param1, 0-10):
 *   0  = off (passthrough)
 *   2  = light (takes the edge off jitter)
 *   4  = moderate (good default)
 *   6  = heavy (noticeable latency)
 *   10 = maximum (very sluggish, mostly for demo)
 *
 * Uses per-axis state to track the EMA between events.
 * Resets after 50ms of inactivity to avoid drift on new movements.
 */

#define DT_DRV_COMPAT zmk_input_processor_smooth

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FP_SHIFT 8  /* 8.8 fixed-point for EMA state */
#define IDLE_RESET_MS 50

/*
 * Map user-facing level (0-10) to internal alpha (256-26).
 * alpha = 256 - level * 23
 * Higher alpha = more responsive. Level 0 → alpha 256 (passthrough).
 */
static const uint16_t level_to_alpha[] = {
	256,  /* 0: off */
	233,  /* 1 */
	210,  /* 2: light */
	187,  /* 3 */
	164,  /* 4: moderate */
	141,  /* 5 */
	118,  /* 6: heavy */
	 95,  /* 7 */
	 72,  /* 8 */
	 49,  /* 9 */
	 26,  /* 10: maximum */
};

struct smooth_data {
	int32_t ema_x;     /* 8.8 fixed-point */
	int32_t ema_y;
	int64_t last_event_time;
	bool primed;
};

static int smooth_handle_event(const struct device *dev, struct input_event *event,
			       uint32_t param1, uint32_t param2,
			       struct zmk_input_processor_state *state)
{
	struct smooth_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	/* Clamp and map level to alpha */
	uint32_t level = MIN(param1, 10);
	if (level == 0) {
		return 0; /* passthrough */
	}
	int32_t alpha = (int32_t)level_to_alpha[level];

	int64_t now = k_uptime_get();

	/* Reset on idle gap */
	if (!data->primed || (now - data->last_event_time) > IDLE_RESET_MS) {
		data->ema_x = 0;
		data->ema_y = 0;
		data->primed = true;
	}
	data->last_event_time = now;

	/* Identify axis */
	int32_t *ema;
	if (event->code == INPUT_REL_X) {
		ema = &data->ema_x;
	} else if (event->code == INPUT_REL_Y) {
		ema = &data->ema_y;
	} else {
		return 0;
	}

	/* EMA in 8.8 fixed-point:
	 * ema = (alpha * new + (256 - alpha) * ema) / 256 */
	int32_t new_fp = (int32_t)event->value << FP_SHIFT;
	*ema = (alpha * new_fp + (256 - alpha) * (*ema)) >> 8;

	event->value = *ema >> FP_SHIFT;

	return 0;
}

static struct zmk_input_processor_driver_api smooth_api = {
	.handle_event = smooth_handle_event,
};

#define SMOOTH_INST(n)                                                 \
	static struct smooth_data smooth_data_##n;                     \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                           \
			      &smooth_data_##n, NULL,                  \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &smooth_api);

DT_INST_FOREACH_STATUS_OKAY(SMOOTH_INST)
