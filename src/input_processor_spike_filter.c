/*
 * Motion smoother input processor (1-euro style, speed-adaptive low-pass)
 *
 * Replaces the old fixed-step "spike filter" slew limiter, which had three
 * fatal flaws for a trackball:
 *   1. Its threshold was a flat constant (not the documented adaptive curve),
 *      so fast flicks were hard-capped at ~25 counts/event -> heavy lag.
 *   2. It kept its output state across motion gaps and never reset it, so the
 *      first event of a *reversed* flick was dragged toward the stale old
 *      value -> the cursor lurched the WRONG way for ~100 ms ("time travel").
 *   3. It had no time term, so behaviour changed with the report rate (e.g.
 *      when BLE bunches reports during a stall).
 *
 * This processor is a proper one-euro filter applied per axis to the relative
 * delta stream:
 *
 *   - It is TIME-AWARE: each event is filtered using the real elapsed time
 *     since the previous event on the SAME axis (k_uptime_get), so it behaves
 *     identically whether reports arrive evenly or in BLE bursts.
 *   - It is SPEED-ADAPTIVE: the low-pass cutoff rises with movement speed, so
 *     slow motion is heavily smoothed (kills stiction jitter / notchiness)
 *     while fast flicks pass through almost untouched (no lag).
 *   - It SELF-RESETS on a motion gap: if no event arrived on an axis for
 *     longer than GAP_RESET_MS, the filter state is discarded and the next
 *     event passes through cleanly. This is what eliminates the stale-state
 *     "time travel" on a direction reversal after a pause.
 *   - It CARRIES the rounding remainder so sub-count slow creep is preserved
 *     rather than quantised away (helps low-speed precision / stiction).
 *
 * cutoff(speed) = mincutoff + beta * speed        [Hz]
 * tau           = 1 / (2*pi*cutoff)
 * alpha         = 1 / (1 + tau/te)                te = elapsed seconds
 * out          += alpha * (in - out)
 *
 * param1 = smoothing strength (1-10)
 *   1  = light  (mincutoff 10 Hz — barely smooths, lowest lag)
 *   6  = balanced
 *   10 = heavy  (mincutoff 1 Hz — strong low-speed smoothing)
 */

#define DT_DRV_COMPAT zmk_input_processor_spike_filter

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#define SF_AXIS_X 0
#define SF_AXIS_Y 1
#define SF_NUM_AXES 2

/* Drop filter state if an axis has been idle longer than this. Slightly
 * longer than a couple of report intervals so normal motion never trips it
 * but a genuine pause + reversal always does. */
#define GAP_RESET_MS 120

/* beta: how fast the cutoff opens up with speed (Hz per count). High enough
 * that a real flick (tens of counts/event) pushes the cutoff well past the
 * report bandwidth, i.e. passes through with negligible lag. */
#define SF_BETA 2.0f

#define SF_TWO_PI 6.28318530718f

struct sf_axis {
	float out;       /* last filtered (smoothed) delta */
	float remainder; /* sub-count carry so slow creep isn't lost */
	int64_t last_ts; /* uptime ms of last event on this axis */
	bool primed;
};

struct sf_data {
	struct sf_axis axis[SF_NUM_AXES];
};

static int sf_handle_event(const struct device *dev, struct input_event *event,
			   uint32_t param1, uint32_t param2,
			   struct zmk_input_processor_state *state)
{
	struct sf_data *data = dev->data;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	int idx;
	if (event->code == INPUT_REL_X) {
		idx = SF_AXIS_X;
	} else if (event->code == INPUT_REL_Y) {
		idx = SF_AXIS_Y;
	} else {
		return 0;
	}

	struct sf_axis *ax = &data->axis[idx];
	float in = (float)event->value;
	int64_t now = k_uptime_get();

	/* Fresh start or a long gap: pass through and (re)prime. This is the
	 * stale-state reset that kills the "time travel" on reversals. */
	if (!ax->primed || (now - ax->last_ts) > GAP_RESET_MS ||
	    (now - ax->last_ts) <= 0) {
		ax->out = in;
		ax->remainder = 0.0f;
		ax->primed = true;
		ax->last_ts = now;
		/* event->value passes through unchanged */
		return 0;
	}

	float te = (float)(now - ax->last_ts) / 1000.0f; /* seconds */
	ax->last_ts = now;

	/* Speed estimate: magnitude of the incoming delta (already a velocity). */
	float speed = in < 0 ? -in : in;

	uint8_t level = (uint8_t)CLAMP(param1, 1, 10);
	float mincutoff = (float)(11 - level); /* 10 Hz (light) .. 1 Hz (heavy) */

	float cutoff = mincutoff + SF_BETA * speed;
	float tau = 1.0f / (SF_TWO_PI * cutoff);
	float alpha = 1.0f / (1.0f + tau / te);

	ax->out += alpha * (in - ax->out);

	/* Emit integer with remainder carry so fractional motion accumulates
	 * instead of being truncated to zero at low speed. */
	float v = ax->out + ax->remainder;
	int emit = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
	ax->remainder = v - (float)emit;

	event->value = emit;
	return 0;
}

static struct zmk_input_processor_driver_api sf_api = {
	.handle_event = sf_handle_event,
};

#define SF_INST(n)                                                     \
	static struct sf_data sf_data_##n;                             \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                           \
			      &sf_data_##n, NULL,                      \
			      POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
			      &sf_api);

DT_INST_FOREACH_STATUS_OKAY(SF_INST)
