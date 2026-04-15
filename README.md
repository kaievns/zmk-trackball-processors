# ZMK Trackball Input Processors

ZMK input processors for trackball-equipped keyboards. Designed for embedded
trackballs on wireless split keyboards where typing vibration, sensor jitter,
and automatic mouse layer management need careful handling.

## Processors

### Spike Filter (`zmk,input-processor-spike-filter`)

Slew rate limiter that clamps how fast deltas can change between consecutive
samples. Catches stiction spikes from the sensor without adding latency to
sustained motion. Per-axis initialisation ensures the first movement on each
axis is always instant.

```
threshold = (11 - level) * 5
```

**param1** = sensitivity level (1-10). Lower = more tolerant, higher = smoother.

```dts
input-processors = <&zip_spike_filter 5>;
```

### Response Curve (`zmk,input-processor-response-curve`)

Controller-style power curve that compresses small movements while preserving
large ones. Uses a pre-computed 256-entry LUT per level with lazy
initialisation, so layer switches are a pointer swap.

```
output = 256 * (input / 256) ^ gamma
```

**param1** = curve intensity (0-10). 0 = linear passthrough, 10 = heavy
compression of small movements.

```dts
input-processors = <&zip_response_curve 6>;
```

### Mouse Layer (`zmk,input-processor-mouse-layer`)

Temporarily activates a configurable layer when the trackball moves, with smart
click/drag/double-click lifecycle management. Handles the full complexity of
embedded trackball UX:

- **Typing lockout**: ignores motion during and shortly after typing
- **Thud filtering**: requires sustained motion (multiple events) to activate,
  rejecting single-event chassis vibrations from key bottom-outs
- **Warm re-activation**: recently deactivated layer re-activates with relaxed
  thresholds for fluid move-click-move-click workflows
- **Instant typing exit**: layer drops immediately when a real key is pressed
- **Held key bypass**: holding a modifier/layer key while moving the trackball
  bypasses the typing lockout (for Alt+Tab, Ctrl+click, etc.)
- **Event suppression**: zeroes motion events during typing lockout to prevent
  cursor drift and save BLE bandwidth
- **Double-click support**: layer lingers briefly after a click to allow
  double-clicks, then deactivates
- **Drag support**: layer stays active during drag operations with a
  movement-aware safety timeout for missed BLE release packets

**param1** = target layer, **param2** = idle timeout (ms)

```dts
input-processors = <&zip_mouse_layer MOUSE 1200>;
```

#### Properties

| Property                 | Default | Description                                             |
| ------------------------ | ------- | ------------------------------------------------------- |
| `require-prior-idle-ms`  | 0       | Minimum ms since last typing burst before activation    |
| `activation-threshold`   | 0       | Minimum cumulative `\|dx\|+\|dy\|` to activate          |
| `activation-window-ms`   | 200     | Time window for threshold accumulation                  |
| `activation-event-min`   | 1       | Minimum separate motion events within window            |
| `double-click-window-ms` | 300     | Linger time after click for double-click                |
| `active-layers`          | []      | Layer indices where activation is allowed (empty = any) |
| `button-positions`       | []      | Key positions treated as mouse buttons                  |

#### Example configuration

```dts
/ {
    zip_mouse_layer {
        require-prior-idle-ms = <1200>;
        activation-threshold = <10>;
        activation-window-ms = <200>;
        activation-event-min = <3>;
        double-click-window-ms = <300>;
        active-layers = <0 1>;  /* HALMAK QWERTY */
        button-positions = <7 16 18>;
    };

    trackball_listener {
        compatible = "zmk,input-listener";
        device = <&split_input>;
        input-processors =
            <&zip_spike_filter 5>,
            <&zip_response_curve 6>,
            <&zip_xy_scaler 2 3>,
            <&zip_mouse_layer MOUSE 1200>;

        scroll {
            layers = <SCROLL>;
            input-processors =
                <&zip_spike_filter 6>,
                <&zip_xy_scaler 1 80>,
                <&zip_xy_to_scroll_mapper>,
                <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>;
        };
    };
};
```

## Usage

Add this repo as a git submodule and reference it in your build:

```makefile
EXTRA_MODULES := /path/to/zmk-trackball-processors
```

### Kconfig

```
CONFIG_ZMK_INPUT_PROCESSOR_SPIKE_FILTER=y
CONFIG_ZMK_INPUT_PROCESSOR_RESPONSE_CURVE=y
CONFIG_ZMK_INPUT_PROCESSOR_MOUSE_LAYER=y
```

## Copyright & License

Everything is released under the terms of the MIT license

(C) 2026 Kai Evans
