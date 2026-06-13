/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_axis_lock

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AXIS_LOCK_MAG_MAX 10000

enum axis_lock_state {
    LOCK_NONE,
    LOCK_VERTICAL,
    LOCK_HORIZONTAL,
};

struct axis_lock_config {
    uint8_t type;
    uint16_t wheel_code;
    uint16_t hwheel_code;
    uint16_t activate_threshold;
    uint16_t release_ms;
};

struct axis_lock_data {
    int32_t mag_v;
    int32_t mag_h;
    int64_t last_event_ms;
    enum axis_lock_state lock;
};

static int32_t abs32(int32_t value) { return value < 0 ? -value : value; }

static int axis_lock_handle_event(const struct device *dev, struct input_event *event,
                                  uint32_t param1, uint32_t param2,
                                  struct zmk_input_processor_state *state) {
    const struct axis_lock_config *cfg = dev->config;
    struct axis_lock_data *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool vertical;
    if (event->code == cfg->wheel_code) {
        vertical = true;
    } else if (event->code == cfg->hwheel_code) {
        vertical = false;
    } else {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();
    int64_t gap = now - data->last_event_ms;
    data->last_event_ms = now;

    if (gap >= cfg->release_ms) {
        data->mag_v = 0;
        data->mag_h = 0;
        data->lock = LOCK_NONE;
    }

    int32_t *mag = vertical ? &data->mag_v : &data->mag_h;
    *mag += abs32(event->value);
    if (*mag > AXIS_LOCK_MAG_MAX) {
        *mag = AXIS_LOCK_MAG_MAX;
    }

    // (Re)decide the lock only at the report boundary (event->sync), after both
    // axes of the report have been accumulated — deciding on the first event of
    // a report would let an X-before-Y jitter spike lock the wrong axis (PMW3610
    // reports X before Y within a report). First fade old motion so the lock
    // tracks RECENT direction. Engaging from NONE only needs activate_threshold;
    // flipping an existing lock to the other axis requires that axis to clearly
    // dominate (>=2x), so deliberate horizontal scrolling still engages even if a
    // vertical wobble grabbed the lock first (a continuous mislock never goes
    // idle, so it would otherwise persist for the whole roll), while incidental
    // cross-axis jitter during a steady scroll never reaches 2x and is suppressed.
    if (event->sync) {
        data->mag_v -= data->mag_v / 4;
        data->mag_h -= data->mag_h / 4;

        enum axis_lock_state winner;
        int32_t win, lose;
        if (data->mag_v >= data->mag_h) {
            winner = LOCK_VERTICAL;
            win = data->mag_v;
            lose = data->mag_h;
        } else {
            winner = LOCK_HORIZONTAL;
            win = data->mag_h;
            lose = data->mag_v;
        }

        if (win >= cfg->activate_threshold &&
            (data->lock == LOCK_NONE || (winner != data->lock && win >= lose * 2))) {
            data->lock = winner;
        }
    }

    // Suppress the minor axis. Also suppress while still NONE (before the first
    // lock): PMW3610 sends X/HWHEEL first with sync=false, ahead of the sync
    // event that decides the lock, so without this the leading event of every
    // fresh gesture would leak a cross-axis delta. The intended axis still
    // passes from the sync event on, since the decision above runs before this.
    if (data->lock == LOCK_NONE || (data->lock == LOCK_VERTICAL && !vertical) ||
        (data->lock == LOCK_HORIZONTAL && vertical)) {
        event->value = 0;
    }

    LOG_DBG("axis lock %d mag_v %d mag_h %d value %d", data->lock, data->mag_v, data->mag_h,
            event->value);

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api axis_lock_driver_api = {
    .handle_event = axis_lock_handle_event,
};

static int axis_lock_init(const struct device *dev) { return 0; }

#define AXIS_LOCK_INST(n)                                                                          \
    static struct axis_lock_data axis_lock_data_##n = {};                                          \
    static const struct axis_lock_config axis_lock_config_##n = {                                  \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .wheel_code = DT_INST_PROP_OR(n, wheel_code, INPUT_REL_WHEEL),                              \
        .hwheel_code = DT_INST_PROP_OR(n, hwheel_code, INPUT_REL_HWHEEL),                           \
        .activate_threshold = DT_INST_PROP_OR(n, activate_threshold, 6),                            \
        .release_ms = DT_INST_PROP_OR(n, release_ms, 200),                                          \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, axis_lock_init, NULL, &axis_lock_data_##n, &axis_lock_config_##n,     \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                         \
                          &axis_lock_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AXIS_LOCK_INST)
