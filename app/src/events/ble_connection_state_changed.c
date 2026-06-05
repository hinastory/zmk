/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zmk/events/ble_connection_state_changed.h>

ZMK_EVENT_IMPL(zmk_ble_connection_state_changed);
