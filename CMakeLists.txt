cmake_minimum_required(VERSION 3.20.0)

# Register this repo as a Zephyr module (see zephyr/module.yml) so the
# out-of-tree PIO drivers in drivers/ (dp_pio and sbw_pio) are processed during Zephyr's
# module loop. That runs before Zephyr captures ZEPHYR_LIBS and builds the
# whole-archive link group; an app-local add_subdirectory() would register the
# driver too late, so their DEVICE_DT_INST_DEFINE device objects would be dropped
# and DEVICE_DT_GET(swd_pio) would fail to link. Must precede find_package().
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

add_subdirectory(lib/dap)

target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
  src/usb/dap_class.cpp
  src/usb/msosv2.cpp
  src/dap/dap_backend.cpp
  src/leds.cpp
  src/pin_owner.cpp
  src/uart_bridge.cpp
  src/autobaud/autobaud.cpp
  src/autobaud/estimator.cpp
)

# The swdp_api adapter (and its C shim over the non-C++-clean swdp.h) is only
# needed for the real SWD backends; the NULL bring-up build excludes it.
if(NOT CONFIG_PROBE_SWD_NULL)
  target_sources(app PRIVATE
    src/dap/swdp_driver.cpp
    src/dap/swdp_shim.c
  )
endif()

# MSP430 Spy-Bi-Wire programming (vendor command 0x90): glue between the PIO
# driver in drivers/sbw_pio/, lib/msp430 and the DAP engine.
if(CONFIG_PROBE_MSP430_SBW)
  add_subdirectory(lib/msp430)
  target_sources(app PRIVATE
    src/msp430/pio_sbw_port.cpp
    src/msp430/msp430_backend.cpp
  )
  target_include_directories(app PRIVATE drivers/sbw_pio)
  target_link_libraries(app PRIVATE msp430)
endif()

target_include_directories(app PRIVATE src)
# The autobaud module (src/autobaud/autobaud.cpp) includes the generated
# autobaud_pio.h (PIO opcodes), which lives alongside the out-of-tree PIO
# driver in drivers/dp_pio/.
target_include_directories(app PRIVATE drivers/dp_pio)
target_link_libraries(app PRIVATE dap)
