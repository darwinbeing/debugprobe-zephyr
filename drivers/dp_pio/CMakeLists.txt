# Only build the PIO driver library when the misc PIO HAL is available
# (CONFIG_SWDP_PIO depends on PIO_RPI_PICO, which needs an enabled
# raspberrypi,pico-pio node). Without it the pico-sdk pio_* symbols are neither
# compiled nor linkable, and a sourceless zephyr_library() would error. The
# driver registers a device only when a raspberrypi,swdp-pio instance exists
# (DT_INST_FOREACH_STATUS_OKAY).
if(CONFIG_SWDP_PIO)
  zephyr_library()
  # swdp_block.cpp: pure builder/parser for the DMA read-block path (also
  # compiled into the host tests).
  zephyr_library_sources(swdp_pio.c swdp_block.cpp)
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

  # This out-of-tree library is included as a Zephyr module (zephyr/module.yml),
  # which runs in Zephyr's module loop early enough for the device objects to be
  # collected into ZEPHYR_LIBS. However it does not automatically inherit the
  # build-order dependency on Zephyr's generated headers (heap_constants.h,
  # offsets.h, syscalls, ...) and can race ahead of header generation on a clean
  # parallel build. Mirror what in-tree libraries get implicitly.
  if(TARGET zephyr_generated_headers)
    add_dependencies(${ZEPHYR_CURRENT_LIBRARY} zephyr_generated_headers)
  endif()
endif()
