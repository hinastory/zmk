/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Conductor host-independent key auto-repeat (typematic).
 *
 * While a non-modifier keyboard-page key is held, the central synthesizes
 * discrete release->press HID report pairs at a fixed interval after an initial
 * delay, so the host sees repeated keypresses regardless of its own key-repeat
 * settings (works around macOS press-and-hold suppressing letter repeat).
 *
 * HID reports are emitted directly via zmk_hid_* + zmk_endpoint_send_report,
 * mirroring pointing/keepalive.c and the pre-release idiom in hid_listener.c —
 * no zmk events are raised, so the implicit/explicit modifier state that
 * hid_listener maintains keeps applying to every re-press.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/conductor_key_repeat.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#if IS_ENABLED(CONFIG_CONDUCTOR_KEY_REPEAT)

#define KEY_REPEAT_DELAY_MIN 100
#define KEY_REPEAT_DELAY_MAX 2000
#define KEY_REPEAT_INTERVAL_MIN 10
#define KEY_REPEAT_INTERVAL_MAX 200

static struct k_work_delayable repeat_work;
static struct k_spinlock lock;

static bool cfg_enabled;
static uint32_t cfg_delay_ms = CONFIG_CONDUCTOR_KEY_REPEAT_DELAY_MS;
static uint32_t cfg_interval_ms = CONFIG_CONDUCTOR_KEY_REPEAT_INTERVAL_MS;

static uint32_t candidate_usage;
static bool has_candidate;

static void repeat_work_handler(struct k_work *work) {
    uint32_t usage;
    bool enabled, active;

    k_spinlock_key_t key = k_spin_lock(&lock);
    enabled = cfg_enabled;
    active = has_candidate;
    usage = candidate_usage;
    k_spin_unlock(&lock, key);

    if (!enabled || !active) {
        return;
    }

    if (!zmk_hid_is_pressed(usage)) {
        key = k_spin_lock(&lock);
        if (has_candidate && candidate_usage == usage) {
            has_candidate = false;
        }
        k_spin_unlock(&lock, key);
        return;
    }

    /* Leave the HID modifier state untouched: release/press the key only, so
     * implicit mods (e.g. LS(N9)) that hid_listener registered keep applying. */
    zmk_hid_release(usage);
    zmk_endpoint_send_report(HID_USAGE_KEY);
    zmk_hid_press(usage);
    zmk_endpoint_send_report(HID_USAGE_KEY);

    uint32_t interval;
    bool reschedule;
    key = k_spin_lock(&lock);
    reschedule = cfg_enabled && has_candidate && candidate_usage == usage;
    interval = cfg_interval_ms;
    k_spin_unlock(&lock, key);

    if (reschedule) {
        k_work_reschedule(&repeat_work, K_MSEC(interval));
    }
}

void zmk_key_repeat_set_config(bool enabled, uint32_t delay_ms, uint32_t interval_ms) {
    if (delay_ms == 0) {
        delay_ms = CONFIG_CONDUCTOR_KEY_REPEAT_DELAY_MS;
    }
    if (delay_ms < KEY_REPEAT_DELAY_MIN) {
        delay_ms = KEY_REPEAT_DELAY_MIN;
    }
    if (delay_ms > KEY_REPEAT_DELAY_MAX) {
        delay_ms = KEY_REPEAT_DELAY_MAX;
    }
    if (interval_ms == 0) {
        interval_ms = CONFIG_CONDUCTOR_KEY_REPEAT_INTERVAL_MS;
    }
    if (interval_ms < KEY_REPEAT_INTERVAL_MIN) {
        interval_ms = KEY_REPEAT_INTERVAL_MIN;
    }
    if (interval_ms > KEY_REPEAT_INTERVAL_MAX) {
        interval_ms = KEY_REPEAT_INTERVAL_MAX;
    }

    bool cancel;
    k_spinlock_key_t key = k_spin_lock(&lock);
    cfg_enabled = enabled;
    cfg_delay_ms = delay_ms;
    cfg_interval_ms = interval_ms;
    cancel = !enabled;
    if (cancel) {
        has_candidate = false;
    }
    k_spin_unlock(&lock, key);

    if (cancel) {
        k_work_cancel_delayable(&repeat_work);
    }

    LOG_DBG("key repeat config: enabled=%d delay=%u interval=%u", (int)enabled, delay_ms,
            interval_ms);
}

void zmk_key_repeat_get_config(bool *enabled, uint32_t *delay_ms, uint32_t *interval_ms) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (enabled) {
        *enabled = cfg_enabled;
    }
    if (delay_ms) {
        *delay_ms = cfg_delay_ms;
    }
    if (interval_ms) {
        *interval_ms = cfg_interval_ms;
    }
    k_spin_unlock(&lock, key);
}

static int key_repeat_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) {
        return 0;
    }
    if (ev->usage_page != HID_USAGE_KEY || is_mod(ev->usage_page, ev->keycode)) {
        return 0;
    }

    uint32_t usage = ZMK_HID_USAGE(ev->usage_page, ev->keycode);

    if (ev->state) {
        bool enabled;
        uint32_t delay;
        k_spinlock_key_t key = k_spin_lock(&lock);
        candidate_usage = usage;
        has_candidate = true;
        enabled = cfg_enabled;
        delay = cfg_delay_ms;
        k_spin_unlock(&lock, key);

        if (enabled) {
            k_work_reschedule(&repeat_work, K_MSEC(delay));
        }
    } else {
        k_spinlock_key_t key = k_spin_lock(&lock);
        bool match = has_candidate && candidate_usage == usage;
        if (match) {
            has_candidate = false;
        }
        k_spin_unlock(&lock, key);

        if (match) {
            k_work_cancel_delayable(&repeat_work);
        }
    }

    return 0;
}

ZMK_LISTENER(conductor_key_repeat, key_repeat_listener);
ZMK_SUBSCRIPTION(conductor_key_repeat, zmk_keycode_state_changed);

static int conductor_key_repeat_init(void) {
    k_work_init_delayable(&repeat_work, repeat_work_handler);
    return 0;
}

SYS_INIT(conductor_key_repeat_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* IS_ENABLED(CONFIG_CONDUCTOR_KEY_REPEAT) */
