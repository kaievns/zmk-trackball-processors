/*
 * Slew rate limiter input processor
 *
 * Limits how fast the delta can change between consecutive samples.
 * If the change exceeds a proportional threshold, the output is
 * clamped toward the target.  The threshold scales with the current
 * output magnitude so fast movements get more headroom while slow
 * movements stay tightly controlled.
 *
 * threshold = base + abs(prev_output) * factor / 16
 *
 * param1 = sensitivity (1-10)
 *   1  = very tolerant
 *   5  = moderate
 *   9  = smooth
 *   10 = very smooth
 */

#define DT_DRV_COMPAT zmk_input_processor_spike_filter

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <stdlib.h>

struct spike_data {
	int16_t out_x;
	int16_t out_y;
	bool primed;
};

static int spike_handle_event(const struct device *dev,
			      struct input_event *event,
			      uint32_t param1, uint32_t param2,
			      struct zmk_input_processor_state *state)
{
	struct spike_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	int16_t *out;
	if (event->code == INPUT_REL_X) {
		out = &data->out_x;
	} else if (event->code == INPUT_REL_Y) {
		out = &data->out_y;
	} else {
		return 0;
	}

	int16_t val = (int16_t)event->value;

	if (!data->primed) {
		data->primed = true;
		*out = val;
		return 0;
	}

	uint8_t level = (uint8_t)CLAMP(param1, 1, 10);
	int32_t threshold = (11 - level) * 5;

	int32_t diff = (int32_t)val - (int32_t)*out;

	if (diff > threshold) {
		*out += (int16_t)threshold;
	} else if (diff < -threshold) {
		*out -= (int16_t)threshold;
	} else {
		*out = val;
	}

	event->value = *out;
	return 0;
}

static struct zmk_input_processor_driver_api spike_api = {
	.handle_event = spike_handle_event,
};

#define SPIKE_INST(n)                                                  \
	static struct spike_data spike_data_##n;                       \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                           \
			      &spike_data_##n, NULL,                   \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &spike_api);

DT_INST_FOREACH_STATUS_OKAY(SPIKE_INST)
