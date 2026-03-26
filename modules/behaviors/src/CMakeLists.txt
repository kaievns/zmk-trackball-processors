target_sources(app PRIVATE
  trackball_profiles.c
)

# Access the PMW3610 driver's public API header
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../drivers/include
)
