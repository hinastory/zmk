/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/split/layer_state.h>
#include <zmk/events/split_layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_split_layer_state_changed);

static uint8_t peripheral_layer = 0;

int zmk_split_peripheral_get_layer(uint8_t *layer) {
    if (!layer) {
        return -EINVAL;
    }
    *layer = peripheral_layer;
    return 0;
}

int zmk_split_peripheral_store_layer(uint8_t layer) {
    peripheral_layer = layer;
    LOG_DBG("Peripheral layer updated to %d", layer);
    return raise_zmk_split_layer_state_changed(
        (struct zmk_split_layer_state_changed){.layer = layer});
}
