/*
 * Acceleration power curve input processor
 *
 * Applies output = d^(exponent/100) to relative X/Y input events.
 * The LUT is pre-computed when the exponent changes.
 *
 *   exponent=100  linear (no acceleration)
 *   exponent=120  mild (MX Ergo-like)
 *   exponent=140  moderate
 *   exponent=200  aggressive
 *
 * Uses the input processor remainder API for sub-pixel precision.
 */

#define DT_DRV_COMPAT zmk_input_processor_accel

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LUT_SIZE  128
#define FP_SHIFT  8  /* 8.8 fixed-point */

struct accel_data {
	uint16_t lut[LUT_SIZE];
	uint16_t current_exponent;
};

/* Lightweight base^exp without math.h */
static float approx_powf(float base, float exponent)
{
	if (base <= 1.0f) return base;

	float m = base;
	int n = 0;
	while (m >= 2.0f) { m *= 0.5f; n++; }

	float log2_m = -1.7417939f + m * (2.8212026f +
			m * (-1.4699568f + m * 0.44717955f));
	float y = exponent * (n + log2_m);

	int yi = (int)y;
	float yf = y - yi;

	float result = 1.0f;
	for (int i = 0; i < yi; i++) result *= 2.0f;

	result *= 1.0f + yf * (0.6931472f + yf * (0.2402265f + yf * 0.0558011f));

	return result;
}

static void rebuild_lut(struct accel_data *data, uint16_t exponent)
{
	float power = exponent / 100.0f;
	if (power < 1.0f) power = 1.0f;

	data->lut[0] = 0;
	for (int d = 1; d < LUT_SIZE; d++) {
		float out = approx_powf((float)d, power);
		uint32_t fp = (uint32_t)(out * (1 << FP_SHIFT) + 0.5f);
		data->lut[d] = (uint16_t)MIN(fp, UINT16_MAX);
	}

	data->current_exponent = exponent;
	LOG_INF("Accel LUT: power=%u/100 lut[5]=%u lut[10]=%u lut[30]=%u",
		exponent, data->lut[5], data->lut[10], data->lut[30]);
}

static int32_t accelerate(const struct accel_data *data, int32_t value)
{
	if (value == 0) return 0;

	int sign = (value > 0) ? 1 : -1;
	uint16_t d = (uint16_t)MIN(abs(value), INT16_MAX);
	uint32_t out;

	if (d < LUT_SIZE) {
		out = data->lut[d];
	} else {
		uint16_t last = data->lut[LUT_SIZE - 1];
		uint16_t prev = data->lut[LUT_SIZE - 2];
		uint16_t slope = last - prev;
		out = last + (uint32_t)slope * (d - LUT_SIZE + 1);
	}

	return sign * (int32_t)out;
}

static int accel_handle_event(const struct device *dev, struct input_event *event,
			      uint32_t param1, uint32_t param2,
			      struct zmk_input_processor_state *state)
{
	struct accel_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	/* Rebuild LUT if exponent changed (layer switch) */
	uint16_t exponent = (uint16_t)param1;
	if (exponent != data->current_exponent) {
		rebuild_lut(data, exponent);
	}

	int32_t accel_fp = accelerate(data, event->value);

	/* Use remainder tracking for sub-pixel precision */
	if (state && state->remainder) {
		accel_fp += *state->remainder;
	}

	int16_t out = (int16_t)(accel_fp >> FP_SHIFT);

	if (state && state->remainder) {
		*state->remainder = (int16_t)(accel_fp - ((int32_t)out << FP_SHIFT));
	}

	event->value = out;
	return 0;
}

static int accel_init(const struct device *dev)
{
	struct accel_data *data = dev->data;
	rebuild_lut(data, 120); /* default MX Ergo-like */
	return 0;
}

static struct zmk_input_processor_driver_api accel_api = {
	.handle_event = accel_handle_event,
};

#define ACCEL_INST(n)                                                  \
	static struct accel_data accel_data_##n;                       \
	DEVICE_DT_INST_DEFINE(n, &accel_init, NULL,                    \
			      &accel_data_##n, NULL,                   \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &accel_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)
