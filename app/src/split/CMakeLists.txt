# Copyright (c) 2022 The ZMK Contributors
# SPDX-License-Identifier: MIT

if (CONFIG_ZMK_SPLIT_BLE)
    add_subdirectory(bluetooth)
endif()

if (CONFIG_ZMK_SPLIT_WIRED)
    add_subdirectory(wired)
endif()

if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE central.c)
    zephyr_linker_sources(SECTIONS ../../include/linker/zmk-split-transport-central.ld)
else()
    target_sources(app PRIVATE peripheral.c)
    if (CONFIG_ZMK_SPLIT_PERIPHERAL_LAYER_STATE)
        target_sources(app PRIVATE peripheral_layer_state.c)
    endif()
    zephyr_linker_sources(SECTIONS ../../include/linker/zmk-split-transport-peripheral.ld)
endif()