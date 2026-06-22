/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

/*
 * Conductor per-device trackball gesture overrides.
 *
 * The trackball gesture processor normally taps a fixed gesture layer (13)
 * binding for each swipe direction. This module adds an optional per-output-
 * endpoint (USB / each BLE profile) override table so the same swipe can emit
 * an OS-appropriate shortcut depending on which device is the current output.
 *
 * Directions use the gesture processor order: 0=up 1=down 2=left 3=right.
 * Endpoints are indexed by zmk_endpoint_instance_to_index() (NONE=0, USB=1,
 * BLE profile P = 2+P). An unset slot falls back to the layer-13 default.
 *
 * The whole feature is opt-in (CONFIG_CONDUCTOR_GESTURE_PROFILE); when disabled
 * or when a direction has no override, gesture output is unchanged.
 */

/* One direction's override as carried over the Studio RPC. present=false means
 * "unset" (the host encodes this as BehaviorBinding.behavior_id < 0) and the
 * firmware falls back to the gesture layer default. */
struct conductor_gesture_dir {
    bool present;
    zmk_behavior_local_id_t local_id;
    uint32_t param1;
    uint32_t param2;
};

/*
 * Processor hook (runs in the input thread): if the feature is enabled and the
 * current output endpoint has an override for `direction`, fills *out with a
 * ready-to-tap binding and returns true. Returns false otherwise (caller then
 * uses the gesture layer default). This is a light, lock-guarded struct copy —
 * no behavior-name lookup happens in input context.
 */
bool conductor_gesture_get_binding(int direction, struct zmk_behavior_binding *out);

/*
 * Snapshot the whole override table for the get_config RPC. Writes up to
 * dirs_cap entries into dirs_out as a row-major [endpoint][4] array and sets
 * *endpoint_count to ZMK_ENDPOINT_COUNT, *active_endpoint to the current output
 * endpoint index, *enabled to the runtime-enable flag. Any out-param may be
 * NULL. Taken under the table lock so it is a consistent snapshot.
 */
void conductor_gesture_snapshot(bool *enabled, uint32_t *active_endpoint, uint32_t *endpoint_count,
                                struct conductor_gesture_dir *dirs_out, size_t dirs_cap);

/*
 * Replace the override table from the set_config RPC and persist it. `dirs` is a
 * row-major [endpoint][4] array `endpoint_count` rows long; rows beyond
 * ZMK_ENDPOINT_COUNT are ignored and missing rows are treated as all-unset.
 * Behavior names are resolved here (RPC thread); the runtime table is swapped
 * under the table lock.
 */
void conductor_gesture_set_config(bool enabled, const struct conductor_gesture_dir *dirs,
                                  uint32_t endpoint_count);

/* Clear the override table and delete the persisted blob. Wired to the Studio
 * reset_settings handler so a factory reset also drops gesture overrides.
 * Returns 0 on success (or if nothing was stored), or a negative errno if the
 * persisted blob could not be deleted. */
int conductor_gesture_reset(void);
