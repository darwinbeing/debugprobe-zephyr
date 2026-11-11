# Entry point of the Zephyr module (zephyr/module.yml). Runs inside Zephyr's
# module loop, early enough for the drivers' device objects to be collected.
add_subdirectory(dp_pio)
add_subdirectory(sbw_pio)
