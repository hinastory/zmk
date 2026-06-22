/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/behavior.h>
#include <zmk/conductor_gesture.h>
#include <zmk/endpoints.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(gesture)

#define GESTURE_RESPONSE(type, ...) ZMK_RPC_RESPONSE(gesture, type, __VA_ARGS__)

#define GESTURE_DIR_COUNT 4

/* The wire (GetConfigResponse/SetConfigRequest.sets) is a nanopb fixed array of
 * 8 (see gesture.options). If a future endpoint layout exceeds that, the per-
 * endpoint sets would silently truncate — fail the build instead. */
BUILD_ASSERT(ZMK_ENDPOINT_COUNT <= 8, "gesture sets max_count (8) < ZMK_ENDPOINT_COUNT");

/* Generic handler names — keep them file-local so they can never collide with a
 * future subsystem's get/set_config. The macro registers the pointer in-TU. */
static zmk_studio_Response get_config(const zmk_studio_Request *req) {
    LOG_DBG("get_config");

    zmk_gesture_GetConfigResponse resp = zmk_gesture_GetConfigResponse_init_zero;

    bool enabled = false;
    uint32_t active_endpoint = 0;
    uint32_t endpoint_count = 0;
    struct conductor_gesture_dir dirs[ZMK_ENDPOINT_COUNT * GESTURE_DIR_COUNT];
    conductor_gesture_snapshot(&enabled, &active_endpoint, &endpoint_count, dirs, ARRAY_SIZE(dirs));

    resp.enabled = enabled;
    resp.endpoint_count = endpoint_count;
    resp.active_endpoint = active_endpoint;

    /* Always return ENDPOINT_COUNT sets, each with 4 dirs, so the host never has
     * to guess. Unset slots carry behavior_id = -1 (host falls back to layer 13). */
    size_t n_ep = MIN((size_t)endpoint_count, ARRAY_SIZE(resp.sets));
    resp.sets_count = n_ep;
    for (size_t ep = 0; ep < n_ep; ep++) {
        zmk_gesture_GestureSet *set = &resp.sets[ep];
        *set = (zmk_gesture_GestureSet)zmk_gesture_GestureSet_init_zero;
        set->dirs_count = GESTURE_DIR_COUNT;
        for (int d = 0; d < GESTURE_DIR_COUNT; d++) {
            const struct conductor_gesture_dir *src = &dirs[ep * GESTURE_DIR_COUNT + d];
            zmk_keymap_BehaviorBinding *b = &set->dirs[d];
            if (src->present) {
                b->behavior_id = (int32_t)src->local_id;
                b->param1 = src->param1;
                b->param2 = src->param2;
            } else {
                b->behavior_id = -1;
                b->param1 = 0;
                b->param2 = 0;
            }
        }
    }

    return GESTURE_RESPONSE(get_config, resp);
}

static zmk_studio_Response set_config(const zmk_studio_Request *req) {
    LOG_DBG("set_config");

    const zmk_gesture_SetConfigRequest *r = &req->subsystem.gesture.request_type.set_config;

    struct conductor_gesture_dir dirs[ZMK_ENDPOINT_COUNT * GESTURE_DIR_COUNT];
    memset(dirs, 0, sizeof(dirs));

    size_t n_ep = MIN((size_t)r->sets_count, (size_t)ZMK_ENDPOINT_COUNT);
    for (size_t ep = 0; ep < n_ep; ep++) {
        const zmk_gesture_GestureSet *set = &r->sets[ep];
        size_t n_dir = MIN((size_t)set->dirs_count, (size_t)GESTURE_DIR_COUNT);
        for (size_t d = 0; d < n_dir; d++) {
            const zmk_keymap_BehaviorBinding *b = &set->dirs[d];
            struct conductor_gesture_dir *dst = &dirs[ep * GESTURE_DIR_COUNT + d];

            /* Sentinel: a negative behavior_id means "unset" -> never cast to the
             * unsigned local id, leave the slot empty (falls back to layer 13). */
            if (b->behavior_id < 0) {
                dst->present = false;
                continue;
            }

            zmk_behavior_local_id_t lid = (zmk_behavior_local_id_t)b->behavior_id;
            if (zmk_behavior_find_behavior_name_from_local_id(lid) == NULL) {
                LOG_WRN("set_config: unknown behavior id %d at [%u][%u] — leaving unset",
                        (int)b->behavior_id, (unsigned)ep, (unsigned)d);
                dst->present = false;
                continue;
            }

            dst->present = true;
            dst->local_id = lid;
            dst->param1 = b->param1;
            dst->param2 = b->param2;
        }
    }

    conductor_gesture_set_config(r->enabled, dirs, (uint32_t)n_ep);

    zmk_gesture_SetConfigResponse resp = zmk_gesture_SetConfigResponse_init_zero;
    resp.ok = true;
    return GESTURE_RESPONSE(set_config, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(gesture, get_config, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gesture, set_config, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int gesture_settings_reset(void) { return conductor_gesture_reset(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(gesture, gesture_settings_reset);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return -ENOTSUP; }
ZMK_RPC_EVENT_MAPPER(gesture, event_mapper);
