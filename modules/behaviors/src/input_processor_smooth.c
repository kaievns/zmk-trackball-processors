/*
 * Moving average smoothing input processor
 *
 * Simple low-pass filter: averages the last N reports per axis.
 * No adaptive alpha, no gap detection, no state resets — every
 * sample gets identical treatment.
 *
 * Both axes share the same ring buffer position so they advance
 * in lockstep, preserving trajectory angle.  If the sensor skips
 * an axis (value is zero), the buffer is zero-filled for that
 * axis to prevent stale data.
 *
 * param1 = window size (number of samples to average)
 *   1  = off (passthrough)
 *   2  = light (~8ms group delay at 8ms report interval)
 *   3  = moderate (~16ms, good default)
 *   4+ = heavier smoothing, more latency
 *   max = 8
 */

#define DT_DRV_COMPAT zmk_input_processor_smooth

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#define MAX_WINDOW 8

struct smooth_data {
	int16_t buf_x[MAX_WINDOW];
	int16_t buf_y[MAX_WINDOW];
	uint8_t pos;        /* current write position in ring buffer */
	uint8_t filled;     /* samples stored so far (up to MAX_WINDOW) */
	bool x_written;     /* X was reported this cycle */
	bool y_written;     /* Y was reported this cycle */
};

static int16_t ring_avg(const int16_t *buf, uint8_t pos, uint8_t n)
{
	int32_t sum = 0;
	for (uint8_t i = 0; i < n; i++) {
		sum += buf[(pos + MAX_WINDOW - i) % MAX_WINDOW];
	}
	return (int16_t)(sum / n);
}

static int smooth_handle_event(const struct device *dev,
			       struct input_event *event,
			       uint32_t param1, uint32_t param2,
			       struct zmk_input_processor_state *state)
{
	struct smooth_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	uint8_t window = (uint8_t)CLAMP(param1, 1, MAX_WINDOW);
	if (window <= 1) {
		return 0;
	}

	int16_t *buf;
	bool *written;

	if (event->code == INPUT_REL_X) {
		buf = data->buf_x;
		written = &data->x_written;
	} else if (event->code == INPUT_REL_Y) {
		buf = data->buf_y;
		written = &data->y_written;
	} else {
		return 0;
	}

	/* Write this axis value into the ring buffer */
	buf[data->pos] = (int16_t)event->value;
	*written = true;

	/* Average over available samples (up to window) */
	uint8_t n = MIN(data->filled + 1, window);
	event->value = ring_avg(buf, data->pos, n);

	/* On sync (last event of report): advance the ring buffer */
	if (event->sync) {
		if (!data->x_written) {
			data->buf_x[data->pos] = 0;
		}
		if (!data->y_written) {
			data->buf_y[data->pos] = 0;
		}

		data->pos = (data->pos + 1) % MAX_WINDOW;
		if (data->filled < MAX_WINDOW) {
			data->filled++;
		}
		data->x_written = false;
		data->y_written = false;
	}

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
