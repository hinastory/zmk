/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_ble_advertising_state_changed {
    uint8_t state;
};

ZMK_EVENT_DECLARE(zmk_ble_advertising_state_changed);
