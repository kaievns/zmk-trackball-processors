# Trackball Profiles Module

Layer-based trackball DPI and acceleration profile switching for ZMK.

Profiles are defined in the keymap devicetree and automatically activated when
the highest active ZMK layer changes. No key bindings required — the right
profile is always applied for the current layer.

## How it works

1. You define profiles in your keymap, each listing the layers it applies to
2. When you switch layers (via `&mo`, `&tog`, `&to`, or auto-activated temp
   layers), the module detects the change
3. The matching profile's DPI and acceleration curve are applied to the
   PMW3610 sensor in real time
4. If no profile matches the current layer, the first profile is used as
   fallback

## Acceleration curves

The `acceleration-exponent` property selects from pre-computed lookup tables:

| Value   | Curve  | Feel                     |
|---------|--------|--------------------------|
| `0`     | Linear | Raw multiplier only      |
| `1-40`  | d^1.3  | Gentle, precise tracking |
| `41-65` | d^1.5  | Moderate acceleration    |
| `66-100`| d^2.0  | Strong, fast flicks      |

Below the `acceleration-threshold` (sensor counts per sample), no curve is
applied — only the base `acceleration-multiplier`. Above it, the selected
power curve kicks in.

## Usage

### 1. Define profiles in your keymap

```dts
#define DEFAULT 0
#define LOWER   1
#define RAISE   2
#define MOUSE   3
#define SCROLL  4
#define GAMEPAD 5

/ {
    trackball_profiles {
        compatible = "zmk,trackball-profiles";

        /* Default profile — used on most layers */
        profile_default {
            layers = <DEFAULT LOWER RAISE MOUSE>;
            dpi = <800>;
            acceleration-multiplier = <100>;  /* 1.0x */
            acceleration-threshold = <5>;
            acceleration-exponent = <50>;     /* d^1.5 curve */
        };

        /* Scroll layer — slow and linear for smooth scrolling */
        profile_scroll {
            layers = <SCROLL>;
            dpi = <400>;
            acceleration-multiplier = <60>;   /* 0.6x */
            acceleration-threshold = <8>;
            acceleration-exponent = <0>;      /* linear */
        };

        /* Gaming — high DPI with strong acceleration */
        profile_gaming {
            layers = <GAMEPAD>;
            dpi = <1600>;
            acceleration-multiplier = <150>;  /* 1.5x */
            acceleration-threshold = <3>;
            acceleration-exponent = <80>;     /* d^2.0 curve */
        };
    };
};
```

### 2. Enable in your .conf

The module auto-enables when `CONFIG_PMW3610=y` and `CONFIG_ZMK_MOUSE=y` are
set. To explicitly control it:

```ini
CONFIG_ZMK_TRACKBALL_PROFILES=y
```

### 3. Layer switching activates profiles

```
Layer 0 (QWERTY) active  →  profile_default  (800 CPI, d^1.5)
Layer 5 (GAMEPAD) active  →  profile_gaming   (1600 CPI, d^2.0)
Layer 4 (SCROLL) active   →  profile_scroll   (400 CPI, linear)
```

The highest active layer determines the profile. A layer can appear in only
one profile's `layers` list. If the same layer appears in multiple profiles,
the first match wins.

### 4. Combine with input processors

This module works alongside ZMK's input processor system. A typical setup uses
`temp_layer` to auto-activate a MOUSE layer on trackball movement, and
per-layer input processors for scroll conversion:

```dts
/ {
    temp_layer: temp_layer {
        compatible = "zmk,input-processor-temp-layer";
        #input-processor-cells = <2>;
        require-prior-idle-ms = <800>;
        excluded-positions = <3 4 5 13 14 15>;
    };

    trackball_listener {
        compatible = "zmk,input-listener";
        device = <&trackball>;
        input-processors = <&temp_layer 3 1200>;  /* MOUSE layer, 1200ms timeout */

        scroll {
            layers = <4>;  /* SCROLL layer */
            input-processors = <&zip_xy_scaler 1 25>,
                               <&zip_xy_to_scroll_mapper>,
                               <&zip_scroll_transform INPUT_TRANSFORM_Y_INVERT>;
        };
    };
};
```

## Building

The module is loaded via `ZMK_EXTRA_MODULES` in the build command:

```bash
west build -s zmk/app -b nice_nano_v2 \
  -DSHIELD=5deg_right \
  -DZMK_CONFIG=/path/to/config \
  -DZMK_EXTRA_MODULES="/path/to/modules/drivers;/path/to/modules/behaviors"
```

## Dependencies

- PMW3610 driver module (provides `pmw3610_set_cpi` and
  `pmw3610_set_acceleration` APIs)
- ZMK with pointing/mouse support (`CONFIG_ZMK_MOUSE=y`)
- ZMK fork with input processors for scroll/temp-layer features
  (`petejohanson/zmk`, branch `feat/pointers-with-input-processors`)
