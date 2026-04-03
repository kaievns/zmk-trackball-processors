/*
 * Acceleration-based spike filter input processor
 *
 * Detects stiction spikes by measuring acceleration (change in delta
 * between consecutive samples).  If the acceleration exceeds a
 * threshold, the sample is replaced with the previous delta.
 *
 * Zero latency on clean samples — values pass through untouched.
 * Only spike samples get replaced.
 *
 * param1 = sensitivity (1-10)
 *   1  = very tolerant (only catches extreme spikes)
 *   5  = moderate
 *   10 = aggressive (catches smaller spikes, may clip fast flicks)
 *
 * Threshold = (11 - level) * 10, so:
 *   level 1 → threshold 100  (accel must exceed 100 to trigger)
 *   level 5 → threshold 60
 *   level 10 → threshold 10
 */

#define DT_DRV_COMPAT zmk_input_processor_spike_filter

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <stdlib.h>

struct spike_data {
	int16_t prev_x;
	int16_t prev_y;
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

	int16_t *prev;
	if (event->code == INPUT_REL_X) {
		prev = &data->prev_x;
	} else if (event->code == INPUT_REL_Y) {
		prev = &data->prev_y;
	} else {
		return 0;
	}

	int16_t val = (int16_t)event->value;

	if (!data->primed) {
		data->primed = true;
		*prev = val;
		return 0;
	}

	uint8_t level = (uint8_t)CLAMP(param1, 1, 10);
	int32_t threshold = (11 - level) * 10;

	int32_t accel = abs((int32_t)val - (int32_t)*prev);

	if (accel > threshold) {
		event->value = *prev;
	}

	/* Always track raw input so comparisons are between
	 * consecutive sensor readings, not filtered output */
	*prev = val;

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
