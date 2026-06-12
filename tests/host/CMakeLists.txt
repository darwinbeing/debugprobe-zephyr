cmake_minimum_required(VERSION 3.20)
project(dap_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

# The library under test (added in Task 1; keep commented until then)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/dap dap)

add_executable(dap_tests
  test_smoke.cpp
)
# target_link_libraries(dap_tests PRIVATE dap)
