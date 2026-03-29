if(CONFIG_ZMK_INPUT_PROCESSOR_ACCEL)
  target_sources(app PRIVATE src/input_processor_accel.c)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_SMOOTH)
  target_sources(app PRIVATE src/input_processor_smooth.c)
endif()

if(CONFIG_ZMK_INPUT_PROCESSOR_MOUSE_LAYER)
  target_sources(app PRIVATE src/input_processor_mouse_layer.c)
endif()

if(CONFIG_ZMK_TRACKBALL_DPI_PROFILES)
  target_sources(app PRIVATE src/trackball_dpi_profiles.c)
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../drivers/include)
endif()
