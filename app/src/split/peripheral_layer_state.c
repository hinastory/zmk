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
#include <zmk/events/split_layer_color_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_split_layer_state_changed);
ZMK_EVENT_IMPL(zmk_split_layer_color_changed);

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

int zmk_split_peripheral_store_layer_color(uint8_t layer_id, uint8_t color_idx) {
    LOG_DBG("Peripheral layer %d color updated to %d", layer_id, color_idx);
    return raise_zmk_split_layer_color_changed(
        (struct zmk_split_layer_color_changed){.layer_id = layer_id, .color_idx = color_idx});
}
