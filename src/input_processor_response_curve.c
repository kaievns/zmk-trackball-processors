/*
 * Response curve input processor — controller-style sensitivity curve
 *
 * Maps input deltas through a power curve that compresses small
 * movements while preserving large ones.  Like a game controller
 * response curve: low values get reduced, high values pass through
 * closer to their original magnitude.
 *
 * The curve is:  output = 256 * (input / 256) ^ gamma
 *
 * Both endpoints are anchored: 0→0, 256→256.  Values above 256
 * are capped.  Sign is preserved.
 *
 * param1 = curve intensity (0-10)
 *   0  = linear (passthrough)
 *   3  = mild compression
 *   5  = moderate
 *   10 = heavy (small moves strongly reduced)
 *
 * A full 256-entry LUT is pre-calculated per level on first use
 * and cached, so layer switches are a pointer swap.
 */

#define DT_DRV_COMPAT zmk_input_processor_response_curve

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <stdlib.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LUT_SIZE 256
#define NUM_LEVELS 11
#define MAX_DELTA 256

/* Gamma per level: higher gamma = more compression of small values.
 * Stored as gamma * 100 to avoid float in the table. */
static const uint16_t level_gamma_x100[] = {
	100,  /* 0: linear */
	104,  /* 1 */
	108,  /* 2 */
	112,  /* 3: mild */
	116,  /* 4 */
	121,  /* 5: moderate */
	127,  /* 6 */
	134,  /* 7 */
	141,  /* 8 */
	150,  /* 9 */
	165,  /* 10: heavy */
};

struct rc_data {
	uint8_t lut[NUM_LEVELS][LUT_SIZE];  /* [level][1..256] → 0..255 maps to 0..256 */
	bool cached[NUM_LEVELS];
};

/* Attempt float pow replacement with integer-friendly approximation.
 * Compute 256 * (d/256)^gamma for d in 1..256, gamma as float. */
static void build_lut(uint8_t *lut, float gamma)
{
	for (int d = 0; d < LUT_SIZE; d++) {
		float norm = (float)(d + 1) / MAX_DELTA;   /* 1/256 .. 256/256 */

		/* Compute norm^gamma via exp(gamma * ln(norm)).
		 * Use a rough log2 + pow2 approach. */
		float m = norm;
		int n = 0;
		while (m < 1.0f) { m *= 2.0f; n--; }
		while (m >= 2.0f) { m *= 0.5f; n++; }

		/* log2(m) for m in [1,2): minimax polynomial */
		float log2_m = -1.7417939f + m * (2.8212026f +
				m * (-1.4699568f + m * 0.44717955f));
		float log2_val = n + log2_m;

		float y = gamma * log2_val;

		/* 2^y via integer + fractional split */
		int yi = (int)y;
		float yf = y - yi;
		if (yf < 0) { yf += 1.0f; yi--; }

		float result = 1.0f;
		if (yi >= 0) {
			for (int i = 0; i < yi; i++) result *= 2.0f;
		} else {
			for (int i = 0; i < -yi; i++) result *= 0.5f;
		}
		result *= 1.0f + yf * (0.6931472f +
			  yf * (0.2402265f + yf * 0.0558011f));

		float out = MAX_DELTA * result;
		int val = (int)(out + 0.5f);
		lut[d] = (uint8_t)CLAMP(val, 0, 255);
	}

	/* Anchor: index 255 (input 256) always maps to 256 → store 255
	 * (we add 1 during lookup to recover the full 1..256 range) */
}

static void ensure_lut(struct rc_data *data, uint8_t level)
{
	if (data->cached[level]) {
		return;
	}
	float gamma = level_gamma_x100[level] / 100.0f;
	build_lut(data->lut[level], gamma);
	data->cached[level] = true;
	LOG_INF("Response curve: built LUT for level %u (gamma=%.2f)", level, (double)gamma);
}

static int rc_handle_event(const struct device *dev,
			   struct input_event *event,
			   uint32_t param1, uint32_t param2,
			   struct zmk_input_processor_state *state)
{
	struct rc_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}
	if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
		return 0;
	}

	uint8_t level = (uint8_t)MIN(param1, NUM_LEVELS - 1);
	if (level == 0) {
		return 0;
	}

	ensure_lut(data, level);

	int32_t val = event->value;
	int sign = (val >= 0) ? 1 : -1;
	int32_t abs_val = abs(val);

	if (abs_val > MAX_DELTA) {
		abs_val = MAX_DELTA;
	}

	/* LUT index: abs_val 1..256 → index 0..255.  abs_val 0 stays 0. */
	int32_t out;
	if (abs_val == 0) {
		out = 0;
	} else {
		out = (int32_t)data->lut[level][abs_val - 1] + 1;
	}

	event->value = sign * out;
	return 0;
}

static struct zmk_input_processor_driver_api rc_api = {
	.handle_event = rc_handle_event,
};

#define RC_INST(n)                                                     \
	static struct rc_data rc_data_##n;                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                           \
			      &rc_data_##n, NULL,                      \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &rc_api);

DT_INST_FOREACH_STATUS_OKAY(RC_INST)
