/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_ble_connection_state_changed {
    bool connected;
    int profile_index;
};

ZMK_EVENT_DECLARE(zmk_ble_connection_state_changed);
