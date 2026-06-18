/*
 * Response curve input processor — controller-style sensitivity curve
 *
 * Maps input deltas through a power curve that compresses small movements
 * while preserving large ones.  Like a game-controller response curve: low
 * values get reduced for fine control, high values pass through close to
 * their original magnitude.
 *
 * The curve is:  output = 256 * (input / 256) ^ gamma   for input in 1..256
 * Above 256 the curve is linear pass-through (a fast flick must never lose
 * distance — the old hard clamp to 256 truncated quick movements).
 *
 * Endpoints are anchored: 0->0, 256->256.  Sign is preserved.
 *
 * param1 = curve intensity (0-10)
 *   0  = linear (pass-through)
 *   3  = mild compression
 *   5  = moderate
 *   10 = heavy (small moves strongly reduced)
 *
 * The LUTs for every level are precomputed once at device init (the target is
 * a Cortex-M4F with a hardware FPU, so powf is cheap and one-time).  Building
 * lazily on the input thread caused a first-move stutter, and the previous
 * hand-rolled log2/exp2 minimax approximation was numerically invalid — it did
 * not satisfy log2(1)=0 / log2(2)=1, producing non-monotonic per-octave cliffs
 * in the LUT.  Using powf directly is both correct and (at init) free.
 */

#define DT_DRV_COMPAT zmk_input_processor_response_curve

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <math.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LUT_SIZE 256
#define NUM_LEVELS 11
#define MAX_DELTA 256

/* Gamma per level: higher gamma = more compression of small values.
 * Stored as gamma * 100 to keep the table integer. */
static const uint16_t level_gamma_x100[NUM_LEVELS] = {
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
	/* lut[level][d] = curved output for input magnitude (d+1), range 0..256. */
	uint16_t lut[NUM_LEVELS][LUT_SIZE];
};

static void build_lut(uint16_t *lut, float gamma)
{
	for (int d = 0; d < LUT_SIZE; d++) {
		float norm = (float)(d + 1) / MAX_DELTA;   /* 1/256 .. 256/256 */
		float out = MAX_DELTA * powf(norm, gamma);
		int val = (int)(out + 0.5f);
		lut[d] = (uint16_t)CLAMP(val, 0, MAX_DELTA);
	}
}

static int rc_init(const struct device *dev)
{
	struct rc_data *data = dev->data;

	for (int level = 0; level < NUM_LEVELS; level++) {
		build_lut(data->lut[level], level_gamma_x100[level] / 100.0f);
	}
	return 0;
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

	int32_t val = event->value;
	int sign = (val >= 0) ? 1 : -1;
	int32_t abs_val = (val < 0) ? -val : val;

	int32_t out;
	if (abs_val == 0) {
		out = 0;
	} else if (abs_val > MAX_DELTA) {
		/* Fast flick: linear pass-through above the curve range so no
		 * distance is lost. */
		out = abs_val;
	} else {
		out = data->lut[level][abs_val - 1];
	}

	event->value = sign * out;
	return 0;
}

static struct zmk_input_processor_driver_api rc_api = {
	.handle_event = rc_handle_event,
};

#define RC_INST(n)                                                     \
	static struct rc_data rc_data_##n;                             \
	DEVICE_DT_INST_DEFINE(n, rc_init, NULL,                        \
			      &rc_data_##n, NULL,                      \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &rc_api);

DT_INST_FOREACH_STATUS_OKAY(RC_INST)
