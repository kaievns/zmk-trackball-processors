if(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL)
  target_sources(app PRIVATE src/input_processor_accel.c)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_SMOOTH)
  target_sources(app PRIVATE src/input_processor_smooth.c)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_RESPONSE_CURVE)
  target_sources(app PRIVATE src/input_processor_response_curve.c)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_MOUSE_LAYER)
  target_sources(app PRIVATE src/input_processor_mouse_layer.c)
endif()

