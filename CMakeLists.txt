cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

add_subdirectory(lib/dap)

target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
  src/usb/dap_class.cpp
  src/usb/msosv2.cpp
  src/dap/dap_backend.cpp
)
target_include_directories(app PRIVATE src)
target_link_libraries(app PRIVATE dap)
