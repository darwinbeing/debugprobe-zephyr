cmake_minimum_required(VERSION 3.20)
project(dap_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/dap dap)

add_executable(dap_tests
  test_smoke.cpp
  test_interfaces.cpp
  test_engine_basics.cpp
  test_connect_swj.cpp
  test_sequences.cpp
  test_transfer.cpp
  test_transfer_block.cpp
  test_autobaud.cpp
  # The autobaud estimator is pure C++ (zero Zephyr deps), so it compiles
  # straight into the host test binary -- identical source as the firmware.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/autobaud/estimator.cpp
)
target_include_directories(dap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(dap_tests PRIVATE dap)
