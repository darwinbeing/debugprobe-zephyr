cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

add_subdirectory(lib/dap)

# Out-of-tree PIO SWD port driver (drivers/dp_pio/). Compiled in-app via
# add_subdirectory + zephyr_library (no module.yml needed); its DT binding lives
# under the app's dts/bindings/, already on the binding search path. The source
# itself is guarded on CONFIG_SWDP_PIO inside that CMakeLists.
zephyr_include_directories(drivers/dp_pio)
add_subdirectory(drivers/dp_pio)

target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
  src/usb/dap_class.cpp
  src/usb/msosv2.cpp
  src/dap/dap_backend.cpp
)

# The swdp_api adapter (and its C shim over the non-C++-clean swdp.h) is only
# needed for the real SWD backends; the NULL bring-up build excludes it.
if(NOT CONFIG_PROBE_SWD_NULL)
  target_sources(app PRIVATE
    src/dap/swdp_driver.cpp
    src/dap/swdp_shim.c
  )
endif()

target_include_directories(app PRIVATE src)
target_link_libraries(app PRIVATE dap)
