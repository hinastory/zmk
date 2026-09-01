/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zmk/behavior.h>

/**
 * @brief Read a macro's current binding list.
 *
 * @param dev A macro behaviour device.
 * @param bindings Set to the macro's binding array.
 * @return The number of bindings, or a negative errno.
 */
int zmk_behavior_macro_get_bindings(const struct device *dev,
                                    const struct zmk_behavior_binding **bindings);

/**
 * @brief Replace a macro's binding list in place.
 *
 * Only safe up to the number of bindings the keymap declared for this macro:
 * the array is sized by devicetree and cannot grow. Callers know that bound
 * from DT_PROP_LEN(node, bindings) and must not exceed it.
 *
 * Recomputes the macro's cached press/release split, so the new sequence
 * takes effect on the next trigger.
 *
 * @return 0 on success, or a negative errno.
 */
int zmk_behavior_macro_set_bindings(const struct device *dev,
                                    const struct zmk_behavior_binding *bindings, size_t count);

/**
 * @brief Default wait/tap spacing this macro was declared with.
 *
 * queue_macro starts every run from these, so an encoder producing bindings
 * for this macro needs them to know when a timing control binding is
 * redundant.
 */
int zmk_behavior_macro_get_defaults(const struct device *dev, uint32_t *wait_ms, uint32_t *tap_ms);
