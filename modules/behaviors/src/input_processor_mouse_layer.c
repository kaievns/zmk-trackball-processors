/*
 * Mouse layer input processor
 *
 * Temporarily activates a configurable layer when the trackball moves,
 * with smart handling of clicks, double-clicks, and drag operations.
 *
 * State machine:
 *   IDLE         → trackball move → ACTIVE (layer on, idle timer starts)
 *   ACTIVE       → idle timeout   → IDLE (layer off)
 *                → button press   → BUTTON_DOWN
 *                → trackball move → reset idle timer
 *   BUTTON_DOWN  → trackball move → DRAGGING
 *                → button release → CLICK_LINGER (DC timer)
 *                → safety timeout → ACTIVE (idle timer, buttons reset)
 *   DRAGGING     → button release → ACTIVE (idle timer)
 *                → safety timeout → ACTIVE (idle timer, buttons reset)
 *   CLICK_LINGER → DC timeout     → IDLE (layer off)
 *                → button press   → BUTTON_DOWN (set pending_deactivate)
 *                → trackball move → ACTIVE (cancel DC, idle timer)
 *
 * After a double-click completes (second release in CLICK_LINGER path),
 * the layer deactivates immediately.
 *
 * Mouse buttons are detected via zmk_position_state_changed for key
 * positions listed in the `button-positions` devicetree property.
 *
 * Uses zmk_keymap_layer_activate/deactivate so the layer is overlaid
 * on whatever was active — deactivation returns to the prior state.
 *
 * param1 = target layer
 * param2 = idle timeout (ms)
 */

#define DT_DRV_COMPAT zmk_input_processor_mouse_layer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zmk/keymap.h>
#include <zmk/events/position_state_changed.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Safety timeout for BUTTON_DOWN/DRAGGING — if a button release is
 * missed (e.g. BLE packet loss on split keyboard), the state machine
 * recovers rather than staying stuck forever. */
#define SAFETY_TIMEOUT_MS 10000

enum ml_state {
	ML_IDLE,
	ML_ACTIVE,
	ML_BUTTON_DOWN,
	ML_DRAGGING,
	ML_CLICK_LINGER,
};

struct ml_config {
	int16_t require_prior_idle_ms;
	int16_t double_click_window_ms;
	uint16_t activation_threshold;
	uint16_t activation_window_ms;
	uint8_t activation_event_min;
	uint32_t active_layer_mask; /* 0 = any layer */
	const uint16_t *button_positions;
	size_t num_button_positions;
};

struct ml_data {
	const struct device *dev;
	enum ml_state state;
	uint8_t target_layer;
	uint32_t idle_timeout_ms;
	int64_t last_typing_ts;
	int64_t last_motion_ts;
	int64_t last_deactivation_ts;
	uint32_t motion_accumulator;
	uint8_t motion_event_count;
	uint8_t buttons_held;
	bool pending_deactivate; /* deactivate on next full release */
	struct k_work_delayable timer_work;
};

/* ── helpers ─────────────────────────────────────────────────────── */

static bool is_button_position(const struct ml_config *cfg, uint32_t position)
{
	for (size_t i = 0; i < cfg->num_button_positions; i++) {
		if (cfg->button_positions[i] == position) {
			return true;
		}
	}
	return false;
}

static void activate_layer(struct ml_data *data)
{
	zmk_keymap_layer_activate(data->target_layer, false);
	LOG_DBG("mouse_layer: layer %d ON", data->target_layer);
}

static void deactivate_layer(struct ml_data *data)
{
	zmk_keymap_layer_deactivate(data->target_layer, false);
	data->state = ML_IDLE;
	data->buttons_held = 0;
	data->pending_deactivate = false;
	data->last_deactivation_ts = k_uptime_get();
	LOG_DBG("mouse_layer: layer %d OFF", data->target_layer);
}

/* ── timer callback (runs in system workqueue) ───────────────────── */

static void timer_cb(struct k_work *work)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(work);
	struct ml_data *data = CONTAINER_OF(dw, struct ml_data, timer_work);

	switch (data->state) {
	case ML_ACTIVE:
		/* idle timeout */
		deactivate_layer(data);
		break;
	case ML_CLICK_LINGER:
		/* double-click window expired without second click */
		deactivate_layer(data);
		break;
	case ML_BUTTON_DOWN:
	case ML_DRAGGING:
		/* Safety timeout — missed button release. Reset to ACTIVE
		 * and let the normal idle timer handle deactivation. */
		LOG_WRN("mouse_layer: safety timeout in %s, recovering",
			data->state == ML_BUTTON_DOWN ? "BUTTON_DOWN" : "DRAGGING");
		data->buttons_held = 0;
		data->pending_deactivate = false;
		data->state = ML_ACTIVE;
		k_work_reschedule(&data->timer_work,
				  K_MSEC(data->idle_timeout_ms));
		break;
	default:
		break;
	}
}

/* ── input processor handle_event (trackball motion) ─────────────── */

static int ml_handle_event(const struct device *dev, struct input_event *event,
			   uint32_t param1, uint32_t param2,
			   struct zmk_input_processor_state *state)
{
	struct ml_data *data = dev->data;
	const struct ml_config *cfg = dev->config;

	if (event->type != INPUT_EV_REL) {
		return 0;
	}

	data->target_layer = (uint8_t)param1;
	data->idle_timeout_ms = param2;

	switch (data->state) {
	case ML_IDLE: {
		if (cfg->active_layer_mask &&
		    !(cfg->active_layer_mask & BIT(zmk_keymap_highest_layer_active()))) {
			return 0;
		}
		int64_t now = k_uptime_get();
		if (cfg->require_prior_idle_ms > 0) {
			if ((data->last_typing_ts + cfg->require_prior_idle_ms) > now) {
				data->motion_accumulator = 0;
				return 0;
			}
		}
		/* Accumulate |dx|+|dy| and event count over the activation
		 * window. Require both magnitude AND event count minimums
		 * before activating. This discriminates between:
		 *  - chassis thuds: high magnitude, 1-2 events, <10ms
		 *  - deliberate motion: sustained events over 15-30ms
		 * A single large impulse from a key bottom-out can't
		 * trip the event count check even if it exceeds the
		 * magnitude threshold. */
		if (cfg->activation_threshold > 0) {
			if ((now - data->last_motion_ts) > cfg->activation_window_ms) {
				data->motion_accumulator = 0;
				data->motion_event_count = 0;
			}
			data->last_motion_ts = now;

			int32_t v = event->value;
			if (v < 0) v = -v;
			data->motion_accumulator += (uint32_t)v;
			data->motion_event_count++;

			/* "Warm" re-activation: if the layer was recently
			 * deactivated (within 2s), the user is likely in a
			 * click workflow (move-click-move-click). Skip the
			 * event count check so small quick moves between
			 * targets can re-activate immediately. The magnitude
			 * threshold still applies.
			 *
			 * "Cold" activation: no recent deactivation — user
			 * was typing or idle. Require both magnitude AND
			 * event count to filter out chassis thuds from key
			 * bottom-outs. */
			bool warm = (now - data->last_deactivation_ts) < 2000;

			if (data->motion_accumulator < cfg->activation_threshold) {
				return 0;
			}
			if (!warm && data->motion_event_count < cfg->activation_event_min) {
				return 0;
			}
		}
		data->motion_accumulator = 0;
		data->motion_event_count = 0;
		activate_layer(data);
		data->state = ML_ACTIVE;
		k_work_reschedule(&data->timer_work, K_MSEC(data->idle_timeout_ms));
		break;
	}

	case ML_ACTIVE:
		/* Re-activate if layer was externally deactivated */
		if (!zmk_keymap_layer_active(data->target_layer)) {
			activate_layer(data);
		}
		k_work_reschedule(&data->timer_work, K_MSEC(data->idle_timeout_ms));
		break;

	case ML_BUTTON_DOWN:
		data->state = ML_DRAGGING;
		/* Start the safety timer fresh now that we know the user
		 * is dragging — movement-aware timeout from here on. */
		k_work_reschedule(&data->timer_work, K_MSEC(SAFETY_TIMEOUT_MS));
		LOG_DBG("mouse_layer: → DRAGGING");
		break;

	case ML_DRAGGING:
		/* User is actively dragging — extend the safety timeout.
		 * This way the 10s safety net only fires when motion has
		 * been absent for 10s straight while a button is held,
		 * which is a much better heuristic for a missed BLE
		 * release packet than a blind hard cap. Real drags of
		 * any length are supported as long as the user keeps
		 * moving. */
		k_work_reschedule(&data->timer_work, K_MSEC(SAFETY_TIMEOUT_MS));
		break;

	case ML_CLICK_LINGER:
		/* movement cancels DC wait, back to normal active */
		k_work_cancel_delayable(&data->timer_work);
		data->state = ML_ACTIVE;
		data->pending_deactivate = false;
		k_work_reschedule(&data->timer_work, K_MSEC(data->idle_timeout_ms));
		LOG_DBG("mouse_layer: move during DC → ACTIVE");
		break;
	}

	return 0;
}

/* ── ZMK event: position state (button tracking + typing idle) ────── */

static int on_position_state(const zmk_event_t *eh)
{
	const struct zmk_position_state_changed *ev =
		as_zmk_position_state_changed(eh);
	if (!ev) {
		return ZMK_EV_EVENT_BUBBLE;
	}

	const struct device *dev = DEVICE_DT_INST_GET(0);
	struct ml_data *data = dev->data;
	const struct ml_config *cfg = dev->config;

	if (is_button_position(cfg, ev->position)) {
		/* ── mouse button press/release ── */
		if (ev->state) {
			switch (data->state) {
			case ML_ACTIVE:
				data->buttons_held++;
				k_work_cancel_delayable(&data->timer_work);
				data->state = ML_BUTTON_DOWN;
				data->pending_deactivate = false;
				k_work_reschedule(&data->timer_work,
						  K_MSEC(SAFETY_TIMEOUT_MS));
				LOG_DBG("mouse_layer: btn press (pos %d) → BUTTON_DOWN",
					ev->position);
				break;
			case ML_CLICK_LINGER:
				data->buttons_held++;
				k_work_cancel_delayable(&data->timer_work);
				data->state = ML_BUTTON_DOWN;
				data->pending_deactivate = true;
				k_work_reschedule(&data->timer_work,
						  K_MSEC(SAFETY_TIMEOUT_MS));
				LOG_DBG("mouse_layer: btn press in DC → BUTTON_DOWN (pending deact)");
				break;
			case ML_BUTTON_DOWN:
			case ML_DRAGGING:
				/* Additional button during click/drag */
				data->buttons_held++;
				break;
			default:
				break;
			}
		} else {
			/* ── button released ── */
			if (data->buttons_held > 0) {
				data->buttons_held--;
			}

			/* only act when all buttons are released */
			if (data->buttons_held > 0) {
				return ZMK_EV_EVENT_BUBBLE;
			}

			switch (data->state) {
			case ML_BUTTON_DOWN:
				if (data->pending_deactivate) {
					/* second click completed → deactivate */
					deactivate_layer(data);
					LOG_DBG("mouse_layer: double-click done → IDLE");
				} else {
					/* first click → linger for DC window */
					data->state = ML_CLICK_LINGER;
					k_work_reschedule(&data->timer_work,
							  K_MSEC(cfg->double_click_window_ms));
					LOG_DBG("mouse_layer: click → CLICK_LINGER (%dms)",
						cfg->double_click_window_ms);
				}
				break;
			case ML_DRAGGING:
				if (data->pending_deactivate) {
					deactivate_layer(data);
					LOG_DBG("mouse_layer: DC drag end → IDLE");
				} else {
					data->state = ML_ACTIVE;
					k_work_reschedule(&data->timer_work,
							  K_MSEC(data->idle_timeout_ms));
					LOG_DBG("mouse_layer: drag end → ACTIVE");
				}
				break;
			default:
				break;
			}
		}
	} else if (ev->state) {
		/*
		 * Non-button keypress — track as typing activity.
		 *
		 * Only non-button positions update the typing timestamp.
		 * This prevents a self-reinforcing lockout: if the mouse
		 * layer is off and the user presses a button position
		 * (producing a base-layer character like "a" instead of
		 * a click), that character must NOT refresh the
		 * require-prior-idle window — otherwise each failed click
		 * attempt blocks reactivation for another 800 ms and the
		 * user can never escape.
		 */
		data->last_typing_ts = ev->timestamp;

		/* Instant typing exit: if a real key is pressed while the
		 * mouse layer is still active (e.g. user clicked a text
		 * input and immediately starts typing), kill the layer
		 * now rather than waiting for the idle or DC timer. Also
		 * clear the grace period so the full thud filter is armed
		 * — typing vibration should not re-activate the layer. */
		if (data->state == ML_ACTIVE || data->state == ML_CLICK_LINGER) {
			k_work_cancel_delayable(&data->timer_work);
			deactivate_layer(data);
			data->last_deactivation_ts = 0;
			LOG_DBG("mouse_layer: instant typing exit");
		}
	}

	return ZMK_EV_EVENT_BUBBLE;
}

/* ── event dispatcher ─────────────────────────────────────────────── */

static int ml_event_dispatcher(const zmk_event_t *eh)
{
	if (as_zmk_position_state_changed(eh)) {
		return on_position_state(eh);
	}
	return ZMK_EV_EVENT_BUBBLE;
}

/* ── init ─────────────────────────────────────────────────────────── */

static int ml_init(const struct device *dev)
{
	struct ml_data *data = dev->data;
	data->dev = dev;
	k_work_init_delayable(&data->timer_work, timer_cb);
	return 0;
}

/* ── driver API ───────────────────────────────────────────────────── */

static const struct zmk_input_processor_driver_api ml_api = {
	.handle_event = ml_handle_event,
};

/* ── ZMK event subscriptions ─────────────────────────────────────── */

ZMK_LISTENER(processor_mouse_layer, ml_event_dispatcher);
ZMK_SUBSCRIPTION(processor_mouse_layer, zmk_position_state_changed);

/* ── device instantiation ─────────────────────────────────────────── */

#define ML_ACTIVE_LAYER_BIT(node, prop, idx) BIT(DT_PROP_BY_IDX(node, prop, idx))

#define ML_ACTIVE_LAYER_MASK(n)                                             \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, active_layers),               \
		(DT_INST_FOREACH_PROP_ELEM_SEP(n, active_layers,            \
			ML_ACTIVE_LAYER_BIT, (|))),                         \
		(0))

#define ML_INST(n)                                                          \
	static struct ml_data ml_data_##n = {};                             \
	static const uint16_t ml_button_positions_##n[] =                   \
		DT_INST_PROP(n, button_positions);                          \
	static const struct ml_config ml_config_##n = {                     \
		.require_prior_idle_ms =                                    \
			DT_INST_PROP_OR(n, require_prior_idle_ms, 0),       \
		.double_click_window_ms =                                   \
			DT_INST_PROP_OR(n, double_click_window_ms, 300),    \
		.activation_threshold =                                     \
			DT_INST_PROP_OR(n, activation_threshold, 0),        \
		.activation_window_ms =                                     \
			DT_INST_PROP_OR(n, activation_window_ms, 200),      \
		.activation_event_min =                                     \
			DT_INST_PROP_OR(n, activation_event_min, 1),        \
		.active_layer_mask = ML_ACTIVE_LAYER_MASK(n),               \
		.button_positions = ml_button_positions_##n,                \
		.num_button_positions =                                     \
			DT_INST_PROP_LEN(n, button_positions),              \
	};                                                                  \
	DEVICE_DT_INST_DEFINE(n, ml_init, NULL,                             \
			      &ml_data_##n, &ml_config_##n,                 \
			      POST_KERNEL,                                  \
			      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,          \
			      &ml_api);

DT_INST_FOREACH_STATUS_OKAY(ML_INST)
