cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
)
target_include_directories(app PRIVATE src)
