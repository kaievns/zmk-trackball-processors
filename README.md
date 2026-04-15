# ZMK Trackball Input Processors

ZMK input processors for trackball-equipped keyboards. Designed for smoothing
and shaping raw sensor deltas before they reach the HID layer.

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

## Usage

Add this repo as a git submodule and reference it in your build:

```makefile
EXTRA_MODULES := /path/to/zmk-trackball-processors
```

### Kconfig

```
CONFIG_ZMK_INPUT_PROCESSOR_SPIKE_FILTER=y
CONFIG_ZMK_INPUT_PROCESSOR_RESPONSE_CURVE=y
```

## Copyright & License

Everything is released under the terms of the MIT license

(C) 2026 Kai Evans
