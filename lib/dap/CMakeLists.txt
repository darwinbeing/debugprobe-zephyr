# Builds as a Zephyr library inside the firmware build, and as a plain
# static library for host unit tests.
set(DAP_SOURCES
  src/engine.cpp
  src/swj.cpp
  src/transfer.cpp
)

if(DEFINED ZEPHYR_BASE)
  zephyr_library_named(dap)
  zephyr_library_sources(${DAP_SOURCES})
  zephyr_library_include_directories(include)
  zephyr_include_directories(include)
  zephyr_library_compile_options(-fno-exceptions -fno-rtti)
else()
  add_library(dap STATIC ${DAP_SOURCES})
  target_include_directories(dap PUBLIC include)
  target_compile_features(dap PUBLIC cxx_std_20)
  target_compile_options(dap PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()
