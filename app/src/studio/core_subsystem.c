/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
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

static zmk_core_RuntimeState build_runtime_state(void) {
    zmk_core_RuntimeState state = zmk_core_RuntimeState_init_zero;

    state.highest_layer = zmk_keymap_highest_layer_active();
    // ZMK omits the implicitly-active default layer from the bitmask; OR it in so the
    // bitmask stays consistent with highest_layer (which always reports >= default).
    state.active_layers_bitmask = zmk_keymap_layer_state() | BIT(zmk_keymap_layer_default());

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

    return state;
}

zmk_studio_Response get_runtime_state(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_core_RuntimeState resp = build_runtime_state();
    return CORE_RESPONSE(get_runtime_state, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(core, get_device_info, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_lock_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, reset_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_ble_profiles, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
#endif
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_tapping_term, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_tapping_term, ZMK_STUDIO_RPC_HANDLER_SECURED);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_RPC_SUBSYSTEM_HANDLER(core, set_ble_profile_name, ZMK_STUDIO_RPC_HANDLER_SECURED);
#endif
ZMK_RPC_SUBSYSTEM_HANDLER(core, get_runtime_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

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

// runtime_state is intentionally REQUEST-ONLY: the host polls get_runtime_state.
// A notification mapper here would fire on boot-time layer/battery events and run the
// protobuf encode + transport TX synchronously on the small boot/main stack -> stack
// overflow before USB enumerates. Polling keeps that work on the larger dedicated RPC
// thread (ZMK_STUDIO_RPC_THREAD_STACK_SIZE), entirely off the boot path.
