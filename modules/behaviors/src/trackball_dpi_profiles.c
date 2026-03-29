/*
 * Layer-based trackball DPI switcher
 *
 * Default DPI applies to all layers. Child nodes override
 * specific layers. Subscribes to ZMK layer state changes
 * and sets the PMW3610 hardware CPI register accordingly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(trackball_dpi, CONFIG_ZMK_LOG_LEVEL);

extern int pmw3610_set_cpi(const struct device *dev, uint16_t cpi);

/* ── Profile data from devicetree ──────────────────────────────────── */

struct dpi_override {
	uint16_t dpi;
	const uint32_t *layers;
	uint8_t layer_count;
};

#define DPI_NODE DT_NODELABEL(trackball_dpi)

#if DT_NODE_EXISTS(DPI_NODE)

static const uint16_t default_dpi = DT_PROP(DPI_NODE, dpi);

#define OVERRIDE_LAYERS(node) \
	static const uint32_t dpi_layers_##node[] = DT_PROP(node, layers);

DT_FOREACH_CHILD(DPI_NODE, OVERRIDE_LAYERS)

#define OVERRIDE_ENTRY(node)                                           \
	{                                                              \
		.dpi         = DT_PROP(node, dpi),                     \
		.layers      = dpi_layers_##node,                      \
		.layer_count = DT_PROP_LEN(node, layers),              \
	},

static const struct dpi_override overrides[] = {
	DT_FOREACH_CHILD(DPI_NODE, OVERRIDE_ENTRY)
};
#define NUM_OVERRIDES ARRAY_SIZE(overrides)

#else

static const uint16_t default_dpi = 600;
static const struct dpi_override overrides[] = {};
#define NUM_OVERRIDES 0

#endif

/* ── State ─────────────────────────────────────────────────────────── */

static const struct device *trackball_dev;
static uint16_t active_dpi;

static uint16_t find_dpi_for_layer(uint8_t layer)
{
	for (int i = 0; i < (int)NUM_OVERRIDES; i++) {
		for (uint8_t j = 0; j < overrides[i].layer_count; j++) {
			if (overrides[i].layers[j] == layer) {
				return overrides[i].dpi;
			}
		}
	}
	return default_dpi;
}

static void apply_dpi(uint16_t dpi)
{
	if (dpi == active_dpi || !trackball_dev) return;
	active_dpi = dpi;
	pmw3610_set_cpi(trackball_dev, dpi);
}

/* ── Layer state listener ──────────────────────────────────────────── */

static int on_layer_state_changed(const zmk_event_t *eh)
{
	uint8_t highest = 0;
	for (uint8_t i = 0; i < 32; i++) {
		if (zmk_keymap_layer_active(i)) {
			highest = i;
		}
	}

	apply_dpi(find_dpi_for_layer(highest));
	return 0;
}

ZMK_LISTENER(trackball_dpi, on_layer_state_changed);
ZMK_SUBSCRIPTION(trackball_dpi, zmk_layer_state_changed);

/* ── Init ──────────────────────────────────────────────────────────── */

static int trackball_dpi_init(void)
{
	trackball_dev = DEVICE_DT_GET_OR_NULL(DT_INST(0, pixart_pmw3610));
	if (!trackball_dev) {
		LOG_WRN("PMW3610 not found — DPI profiles disabled");
		return 0;
	}

	active_dpi = 0;
	apply_dpi(default_dpi);
	LOG_INF("Trackball DPI: default=%u, %u overrides",
		default_dpi, (unsigned)NUM_OVERRIDES);
	return 0;
}

SYS_INIT(trackball_dpi_init, APPLICATION, 99);
