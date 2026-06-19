/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <pb_encode.h>
#include <zmk/ble.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>
#include <zmk/conductor_os.h>
#include <zmk/endpoints.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

ZMK_RPC_SUBSYSTEM(core)

#define CORE_RESPONSE(type, ...) ZMK_RPC_RESPONSE(core, type, __VA_ARGS__)

static bool encode_device_info_name(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, CONFIG_ZMK_KEYBOARD_NAME, strlen(CONFIG_ZMK_KEYBOARD_NAME));
}

#if IS_ENABLED(CONFIG_HWINFO)
static bool encode_device_info_serial_number(pb_ostream_t *stream, const pb_field_t *field,
                                             void *const *arg) {
    uint8_t id_buffer[32];
    const ssize_t id_size = hwinfo_get_device_id(id_buffer, ARRAY_SIZE(id_buffer));

    if (id_size <= 0) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, id_buffer, id_size);
}

#endif // IS_ENABLED(CONFIG_HWINFO)

static bool encode_device_info_firmware_version(pb_ostream_t *stream, const pb_field_t *field,
                                                void *const *arg) {
    const char *ver = CONFIG_ZMK_STUDIO_FIRMWARE_VERSION;
    if (!ver || ver[0] == '\0') {
        return true; // Skip if empty
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    /* Append build date/time to version string so users can identify
     * exactly which firmware build is running. Format: "<ver> (YYYY-MM-DD HH:MM)" */
    static char ver_with_build[64];
    /* Parse __DATE__ ("Mon DD YYYY") to ISO format */
    const char *d = __DATE__;
    const char *t = __TIME__;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon_idx = 0;
    for (int i = 0; i < 12; i++) {
        if (d[0] == months[i*3] && d[1] == months[i*3+1] && d[2] == months[i*3+2]) {
            mon_idx = i + 1;
            break;
        }
    }
    snprintf(ver_with_build, sizeof(ver_with_build), "%s (%c%c%c%c-%02d-%c%c %c%c:%c%c)",
             ver,
             d[7], d[8], d[9], d[10],           /* year */
             mon_idx,                            /* month */
             d[4] == ' ' ? '0' : d[4], d[5],    /* day */
             t[0], t[1], t[3], t[4]);           /* HH:MM */

    return pb_encode_string(stream, ver_with_build, strlen(ver_with_build));
}

zmk_studio_Response get_device_info(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_GetDeviceInfoResponse resp = zmk_core_GetDeviceInfoResponse_init_zero;

    resp.name.funcs.encode = encode_device_info_name;
#if IS_ENABLED(CONFIG_HWINFO)
    resp.serial_number.funcs.encode = encode_device_info_serial_number;
#endif // IS_ENABLED(CONFIG_HWINFO)
    resp.firmware_version.funcs.encode = encode_device_info_firmware_version;

    return CORE_RESPONSE(get_device_info, resp);
}

zmk_studio_Response get_lock_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_LockState resp = zmk_studio_core_get_lock_state();

    return CORE_RESPONSE(get_lock_state, resp);
}

zmk_studio_Response reset_settings(const zmk_studio_Request *req) {
    LOG_DBG("");
    ZMK_RPC_SUBSYSTEM_SETTINGS_RESET_FOREACH(sub) {
        int ret = sub->callback();
        if (ret < 0) {
            LOG_ERR("Failed to reset settings: %d", ret);
            return CORE_RESPONSE(reset_settings, false);
        }
    }

    return CORE_RESPONSE(reset_settings, true);
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
zmk_studio_Response get_ble_profiles(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_GetBleProfilesResponse resp = zmk_core_GetBleProfilesResponse_init_zero;

    resp.active_index = zmk_ble_active_profile_index();
    resp.profiles_count = 0;

    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT && i < 5; i++) {
        zmk_core_BleProfileInfo *p = &resp.profiles[i];
        bool is_open = zmk_ble_profile_is_open(i);

        if (!is_open) {
            char *name = zmk_ble_profile_name(i);
            if (name && name[0] != '\0') {
                strncpy(p->name, name, sizeof(p->name) - 1);
            } else {
                // No device name stored — use BLE address as fallback
                bt_addr_le_t *addr = zmk_ble_profile_address(i);
                bt_addr_le_to_str(addr, p->name, sizeof(p->name));
            }
            p->name[sizeof(p->name) - 1] = '\0';
        }
        p->connected = zmk_ble_profile_is_connected(i);
        resp.profiles_count++;
    }

    return CORE_RESPONSE(get_ble_profiles, resp);
}
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */

zmk_studio_Response get_tapping_term(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_GetTappingTermResponse resp = zmk_core_GetTappingTermResponse_init_zero;
    int32_t override = zmk_hold_tap_get_tapping_term();
    resp.tapping_term_ms = (override >= 0) ? override : zmk_hold_tap_get_default_tapping_term();
    resp.default_tapping_term_ms = zmk_hold_tap_get_default_tapping_term();
    return CORE_RESPONSE(get_tapping_term, resp);
}

zmk_studio_Response set_tapping_term(const zmk_studio_Request *req) {
    LOG_DBG("");
    uint32_t ms = req->subsystem.core.request_type.set_tapping_term.tapping_term_ms;
    zmk_hold_tap_set_tapping_term((int32_t)ms);
    return CORE_RESPONSE(set_tapping_term, true);
}

zmk_studio_Response get_os_config(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_GetOsConfigResponse resp = zmk_core_GetOsConfigResponse_init_zero;

#if IS_ENABLED(CONFIG_CONDUCTOR_OS_PROFILE)
    bool enabled = false;
    uint8_t map[ZMK_ENDPOINT_COUNT];
    size_t len = ARRAY_SIZE(map);
    int active_endpoint = 0;
    uint8_t active_layer = 0;

    // os_map / active_os carry per-endpoint overlay-layer IDs (wire names kept);
    // these are stable zmk_keymap_layer_id values, NOT ordered indices.
    conductor_profile_get_config(&enabled, map, &len, &active_endpoint, &active_layer);

    resp.enabled = enabled;
    resp.endpoint_count = (uint32_t)len;
    resp.active_endpoint = (uint32_t)active_endpoint;
    resp.active_os = (uint32_t)active_layer;

    size_t n = MIN(len, ARRAY_SIZE(resp.os_map));
    resp.os_map_count = n;
    for (size_t i = 0; i < n; i++) {
        resp.os_map[i] = map[i];
    }
#endif

    return CORE_RESPONSE(get_os_config, resp);
}

zmk_studio_Response set_os_config(const zmk_studio_Request *req) {
    LOG_DBG("");
#if IS_ENABLED(CONFIG_CONDUCTOR_OS_PROFILE)
    const zmk_core_SetOsConfigRequest *r = &req->subsystem.core.request_type.set_os_config;

    uint8_t map[ZMK_ENDPOINT_COUNT];
    size_t n = MIN((size_t)r->os_map_count, ARRAY_SIZE(map));
    for (size_t i = 0; i < n; i++) {
        map[i] = (uint8_t)r->os_map[i];
    }

    conductor_profile_set_config(r->enabled, map, n);
#endif
    return CORE_RESPONSE(set_os_config, true);
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
zmk_studio_Response set_ble_profile_name(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_core_SetBleProfileNameRequest *r =
        &req->subsystem.core.request_type.set_ble_profile_name;
    if (r->profile_index >= ZMK_BLE_PROFILE_COUNT) {
        return CORE_RESPONSE(set_ble_profile_name, false);
    }
    int err = zmk_ble_set_profile_name((uint8_t)r->profile_index, r->name);
    return CORE_RESPONSE(set_ble_profile_name, (err == 0));
}
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */

// Active-layer bitmask with the implicit default layer OR'd in, and the
// per-profile overlay layer (if any) masked out so a persistent overlay is
// treated as part of the base instead of polluting the reported "highest"
// layer (the host uses highest_layer for the mouse-layer indicator and the
// live keymap overlay).
static zmk_keymap_layers_state_t effective_layer_mask(void) {
    zmk_keymap_layers_state_t mask = zmk_keymap_layer_state() | BIT(zmk_keymap_layer_default());
#if IS_ENABLED(CONFIG_CONDUCTOR_OS_PROFILE)
    uint8_t profile_overlay = 0;
    conductor_profile_get_config(NULL, NULL, NULL, NULL, &profile_overlay);
    if (profile_overlay != 0) {
        mask &= ~BIT(profile_overlay);
    }
#endif
    return mask;
}

static uint8_t effective_highest_layer(void) {
#if IS_ENABLED(CONFIG_CONDUCTOR_OS_PROFILE)
    uint8_t overlay_id = 0;
    conductor_profile_get_config(NULL, NULL, NULL, NULL, &overlay_id);
    if (overlay_id != 0) {
        // Mirror zmk_keymap_highest_layer_active() (highest active INDEX, not id —
        // correct under CONFIG_ZMK_KEYMAP_LAYER_REORDERING) but skip the
        // per-profile overlay so it doesn't pollute the host-facing highest layer.
        for (int idx = (int)ZMK_KEYMAP_LAYERS_LEN - 1; idx >= 0; idx--) {
            zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)idx);
            if (id == ZMK_KEYMAP_LAYER_ID_INVAL || id == overlay_id) {
                continue;
            }
            if (zmk_keymap_layer_active(id)) {
                return (uint8_t)idx;
            }
        }
        return 0;
    }
#endif
    return zmk_keymap_highest_layer_active();
}

static zmk_core_RuntimeState build_runtime_state(void) {
    zmk_core_RuntimeState state = zmk_core_RuntimeState_init_zero;

    state.active_layers_bitmask = effective_layer_mask();
    state.highest_layer = effective_highest_layer();

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    state.battery_central = zmk_battery_state_of_charge();
#else
    state.battery_central = 255; // unknown
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t periph_level = 0;
    // Polled only (after connect), so the peripheral has normally reported by now.
    if (zmk_split_central_get_peripheral_battery_level(0, &periph_level) == 0) {
        state.battery_peripheral = periph_level;
    } else {
        state.battery_peripheral = 255; // unknown / not configured
    }
#else
    state.battery_peripheral = 255; // unknown
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
    state.charging = zmk_usb_is_powered();
#else
    state.charging = false;
#endif

#if IS_ENABLED(CONFIG_CONDUCTOR_OS_PROFILE)
    // active_os carries the currently-applied overlay layer (wire name kept).
    uint8_t active_layer = 0;
    conductor_profile_get_config(NULL, NULL, NULL, NULL, &active_layer);
    state.active_os = (uint32_t)active_layer;
    state.os_profile_enabled = conductor_profile_layers_enabled();
#else
    state.active_os = 0;
    state.os_profile_enabled = false;
#endif

    return state;
}

zmk_studio_Response get_runtime_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_RuntimeState resp = build_runtime_state();
    return CORE_RESPONSE(get_runtime_state, resp);
}

// --- Live input event stream (boot-safe, off the keyscan stack) ---
//
// Subscription gate. Defaults to 0 so NOTHING is emitted at boot: the keyscan-thread
// listeners below early-return until the host explicitly enables the stream, and the
// worker thread sits idle on an empty queue. This keeps the boot/main stack clear of
// any protobuf encode + transport TX, exactly like the runtime_state request-only design.
static atomic_t input_sub = ATOMIC_INIT(0);

#define INPUT_EVT_KIND_KEY 0
#define INPUT_EVT_KIND_LAYER 1

struct input_evt {
    uint8_t kind;
    uint32_t position;
    bool pressed;
    uint32_t highest_layer;
};

K_MSGQ_DEFINE(input_evt_q, sizeof(struct input_evt), 32, 4);

zmk_studio_Response subscribe_input(const zmk_studio_Request *req) {
    LOG_DBG("");
    bool enable = req->subsystem.core.request_type.subscribe_input.enable;
    atomic_set(&input_sub, enable ? 1 : 0);
    if (!enable) {
        // Drop anything already queued so a re-subscribe starts clean.
        k_msgq_purge(&input_evt_q);
    }
    return CORE_RESPONSE(subscribe_input, true);
}

// Runs on the keyscan/main thread (small stack). MUST do nothing heavy: gate check +
// non-blocking msgq put only. Drops on full (best-effort: a missed key flash is fine).
static int input_position_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (atomic_get(&input_sub) == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct input_evt item = {
        .kind = INPUT_EVT_KIND_KEY,
        .position = ev->position,
        .pressed = ev->state,
    };
    k_msgq_put(&input_evt_q, &item, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

// Also on the keyscan/main thread; same constraints as above.
static int input_layer_listener(const zmk_event_t *eh) {
    if (atomic_get(&input_sub) == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct input_evt item = {
        .kind = INPUT_EVT_KIND_LAYER,
        .highest_layer = effective_highest_layer(),
    };
    k_msgq_put(&input_evt_q, &item, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(core_input_position, input_position_listener);
ZMK_SUBSCRIPTION(core_input_position, zmk_position_state_changed);

ZMK_LISTENER(core_input_layer, input_layer_listener);
ZMK_SUBSCRIPTION(core_input_layer, zmk_layer_state_changed);

// Dedicated worker thread. Raising the studio core events here makes the standard
// studio_rpc listener encode + TX run on THIS thread's stack (sized to match
// CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE), never on the keyscan/boot stack.
static void input_stream_worker(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    for (;;) {
        struct input_evt item;
        k_msgq_get(&input_evt_q, &item, K_FOREVER);

        // Re-check the gate: an unsubscribe may have arrived after this item was queued.
        if (atomic_get(&input_sub) == 0) {
            continue;
        }

        if (item.kind == INPUT_EVT_KIND_KEY) {
            raise_zmk_studio_core_input_key_event((struct zmk_studio_core_input_key_event){
                .position = item.position, .pressed = item.pressed});
        } else {
            raise_zmk_studio_core_layer_state_event(
                (struct zmk_studio_core_layer_state_event){.highest_layer = item.highest_layer});
        }
    }
}

K_THREAD_DEFINE(core_input_stream_thread, CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE,
                input_stream_worker, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

ZMK_RPC_SUBSYSTEM_HANDLER(core, get_device_info, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_lock_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, reset_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_ble_profiles, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
#endif
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_tapping_term, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_tapping_term, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_os_config, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_os_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_ble_profile_name, ZMK_STUDIO_RPC_HANDLER_SECURED);
#endif
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_runtime_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, subscribe_input, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

static int core_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_studio_core_lock_state_changed *lock_ev = as_zmk_studio_core_lock_state_changed(eh);

    if (!lock_ev) {
        return -ENOTSUP;
    }

    LOG_DBG("Mapped a lock state event properly");

    *n = ZMK_RPC_NOTIFICATION(core, lock_state_changed, lock_ev->state);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(core, core_event_mapper, zmk_studio_core_lock_state_changed);

// These mappers run inside studio_rpc_listener_cb, which is invoked synchronously on the
// thread that raised the event. The two events below are ONLY raised from
// input_stream_worker, so the encode + TX happens on the worker's 4096 stack.
static int core_input_key_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_studio_core_input_key_event *ev = as_zmk_studio_core_input_key_event(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    zmk_core_KeyInputEvent input_key = zmk_core_KeyInputEvent_init_zero;
    input_key.position = ev->position;
    input_key.pressed = ev->pressed;

    *n = ZMK_RPC_NOTIFICATION(core, input_key, input_key);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(core_input_key, core_input_key_event_mapper, zmk_studio_core_input_key_event);

static int core_layer_state_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    struct zmk_studio_core_layer_state_event *ev = as_zmk_studio_core_layer_state_event(eh);

    if (!ev) {
        return -ENOTSUP;
    }

    zmk_core_LayerStateChanged layer_changed = zmk_core_LayerStateChanged_init_zero;
    layer_changed.highest_layer = ev->highest_layer;

    *n = ZMK_RPC_NOTIFICATION(core, layer_changed, layer_changed);
    return 0;
}

ZMK_RPC_EVENT_MAPPER(core_layer_state, core_layer_state_event_mapper,
                     zmk_studio_core_layer_state_event);

// runtime_state is intentionally REQUEST-ONLY: the host polls get_runtime_state.
// A notification mapper here would fire on boot-time layer/battery events and run the
// protobuf encode + transport TX synchronously on the small boot/main stack -> stack
// overflow before USB enumerates. Polling keeps that work on the larger dedicated RPC
// thread (ZMK_STUDIO_RPC_THREAD_STACK_SIZE), entirely off the boot path.
