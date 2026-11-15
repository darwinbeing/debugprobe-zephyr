cmake_minimum_required(VERSION 3.20)
project(dap_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/dap dap)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/msp430 msp430)

add_executable(dap_tests
  test_smoke.cpp
  test_interfaces.cpp
  test_engine_basics.cpp
  test_connect_swj.cpp
  test_sequences.cpp
  test_transfer.cpp
  test_transfer_block.cpp
  test_vendor_hook.cpp
  test_autobaud.cpp
  test_sbw_tap.cpp
  test_cpuxv2.cpp
  test_msp430_session.cpp
  test_msp430_protocol.cpp
  test_fake_tap.cpp
  test_debugger.cpp
  test_msp430_session_debug.cpp
  test_sbw_pack.cpp
  test_pin_owner.cpp
  # The autobaud estimator is pure C++ (zero Zephyr deps), so it compiles
  # straight into the host test binary -- identical source as the firmware.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/autobaud/estimator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/pin_owner.cpp
)
target_include_directories(dap_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/sbw_pio)
target_link_libraries(dap_tests PRIVATE dap msp430)
