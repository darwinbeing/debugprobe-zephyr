if(CONFIG_SBW_PIO)
  zephyr_library()
  zephyr_library_sources(sbw_pio.c)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

  # Same build-order fix as drivers/dp_pio: an out-of-tree module library does
  # not inherit the dependency on Zephyr's generated headers.
  if(TARGET zephyr_generated_headers)
    add_dependencies(${ZEPHYR_CURRENT_LIBRARY} zephyr_generated_headers)
  endif()
endif()
