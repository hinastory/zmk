/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <dt-bindings/zmk/pointing.h>

typedef uint8_t zmk_mouse_button_flags_t;
typedef uint16_t zmk_mouse_button_t;

/**
 * Toggle the scroll invert (natural scrolling) setting.
 * Used by the &scroll_invert_toggle behavior so a key can flip
 * scroll direction without going through Studio. Persists to settings.
 */
void zmk_pointing_toggle_scroll_invert(void);

/**
 * Toggle the Auto Mouse Layer (AML) enabled state.
 * Used by the &aml_toggle behavior so a key/combo can enable or disable AML
 * without going through Studio. Persists to settings so it survives reboot.
 */
void zmk_pointing_toggle_aml(void);
