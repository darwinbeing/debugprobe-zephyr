# Builds as a Zephyr library inside the firmware build, and as a plain
# static library for host unit tests (same pattern as lib/dap).
set(MSP430_SOURCES
  src/sbw_tap.cpp
  src/cpuxv2.cpp
  src/funclets.cpp
)

if(DEFINED ZEPHYR_BASE)
  zephyr_library_named(msp430)
  zephyr_library_sources(${MSP430_SOURCES})
  zephyr_library_include_directories(include)
  zephyr_include_directories(include)
  zephyr_library_compile_options(-fno-exceptions -fno-rtti)
else()
  add_library(msp430 STATIC ${MSP430_SOURCES})
  target_include_directories(msp430 PUBLIC include)
  target_compile_features(msp430 PUBLIC cxx_std_20)
  target_compile_options(msp430 PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()
