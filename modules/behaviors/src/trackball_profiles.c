/*
 * Layer-based trackball profile switcher
 *
 * Subscribes to ZMK layer state changes and automatically applies
 * the matching trackball profile (DPI + acceleration curve).
 *
 * Profiles are defined in the keymap devicetree:
 *
 *   trackball_profiles {
 *       compatible = "zmk,trackball-profiles";
 *       profile_default {
 *           layers = <0 1 2>;        // DEFAULT, LOWER, RAISE
 *           dpi = <800>;
 *           acceleration-multiplier = <100>;
 *           acceleration-threshold = <5>;
 *           acceleration-exponent = <50>;
 *       };
 *       profile_gaming {
 *           layers = <3>;            // GAMEPAD
 *           dpi = <1600>;
 *           acceleration-multiplier = <150>;
 *           acceleration-threshold = <3>;
 *           acceleration-exponent = <80>;
 *       };
 *   };
 *
 * When the highest active layer changes, the profile that lists
 * that layer is activated. If no profile matches, profile 0 is used.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <pmw3610_api.h>

LOG_MODULE_REGISTER(trackball_profiles, CONFIG_ZMK_LOG_LEVEL);

/* ── Profile data from devicetree ──────────────────────────────────── */

struct tb_profile {
	uint16_t dpi;
	uint16_t multiplier;
	uint16_t threshold;
	uint16_t exponent;
	const uint32_t *layers;
	uint8_t layer_count;
};

#define TB_PROFILES_NODE DT_NODELABEL(trackball_profiles)

#if DT_NODE_EXISTS(TB_PROFILES_NODE)

/* Collect layer arrays for each profile child */
#define PROFILE_LAYERS(node) \
	static const uint32_t profile_layers_##node[] = DT_PROP(node, layers);

DT_FOREACH_CHILD(TB_PROFILES_NODE, PROFILE_LAYERS)

#define PROFILE_ENTRY(node)                                            \
	{                                                              \
		.dpi        = DT_PROP(node, dpi),                      \
		.multiplier = DT_PROP(node, acceleration_multiplier),  \
		.threshold  = DT_PROP(node, acceleration_threshold),   \
		.exponent   = DT_PROP(node, acceleration_exponent),    \
		.layers     = profile_layers_##node,                   \
		.layer_count = DT_PROP_LEN(node, layers),              \
	},

static const struct tb_profile profiles[] = {
	DT_FOREACH_CHILD(TB_PROFILES_NODE, PROFILE_ENTRY)
};
#define NUM_PROFILES ARRAY_SIZE(profiles)

#else /* no profiles node */

static const uint32_t default_layers[] = { 0 };
static const struct tb_profile profiles[] = {
	{ .dpi = 800, .multiplier = 100, .threshold = 5, .exponent = 50,
	  .layers = default_layers, .layer_count = 1 },
};
#define NUM_PROFILES 1

#endif

/* ── PMW3610 device reference ──────────────────────────────────────── */

static const struct device *trackball_dev;
static int active_profile_idx = -1;

static void apply_profile(int idx)
{
	if (idx == active_profile_idx || !trackball_dev) return;
	if (idx < 0 || idx >= (int)NUM_PROFILES) idx = 0;

	const struct tb_profile *p = &profiles[idx];
	active_profile_idx = idx;

	pmw3610_set_cpi(trackball_dev, p->dpi);
	pmw3610_set_acceleration(trackball_dev,
				 p->multiplier,
				 p->threshold,
				 p->exponent);

	LOG_INF("Trackball profile %d: DPI=%u mul=%u thr=%u exp=%u",
		idx, p->dpi, p->multiplier, p->threshold, p->exponent);
}

/* Find profile matching the highest active layer */
static int find_profile_for_layer(uint8_t layer)
{
	for (int i = 0; i < (int)NUM_PROFILES; i++) {
		for (uint8_t j = 0; j < profiles[i].layer_count; j++) {
			if (profiles[i].layers[j] == layer) {
				return i;
			}
		}
	}
	return 0; /* fallback to first profile */
}

/* ── Layer state listener ──────────────────────────────────────────── */

static int on_layer_state_changed(const zmk_event_t *eh)
{
	/* Find the highest active layer */
	uint8_t highest = 0;
	for (uint8_t i = 0; i < 32; i++) {
		if (zmk_keymap_layer_active(i)) {
			highest = i;
		}
	}

	int idx = find_profile_for_layer(highest);
	apply_profile(idx);

	return 0;
}

ZMK_LISTENER(trackball_profiles, on_layer_state_changed);
ZMK_SUBSCRIPTION(trackball_profiles, zmk_layer_state_changed);

/* ── Init ──────────────────────────────────────────────────────────── */

static int trackball_profiles_init(void)
{
	trackball_dev = DEVICE_DT_GET_OR_NULL(DT_INST(0, pixart_pmw3610));
	if (!trackball_dev) {
		LOG_WRN("PMW3610 device not found — profiles disabled");
		return 0;
	}

	LOG_INF("Trackball profiles: %u profiles loaded", (unsigned)NUM_PROFILES);
	apply_profile(0);
	return 0;
}

SYS_INIT(trackball_profiles_init, APPLICATION, 99);
