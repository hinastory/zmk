/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

// Active layer index synchronized from the central to a split peripheral.
// On the peripheral, get returns the last value received over the split link.
int zmk_split_peripheral_get_layer(uint8_t *layer);

int zmk_split_peripheral_store_layer(uint8_t layer);
