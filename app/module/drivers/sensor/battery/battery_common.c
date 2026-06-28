/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <zephyr/drivers/sensor.h>

#include "battery_common.h"

int battery_channel_get(const struct battery_value *value, enum sensor_channel chan,
                        struct sensor_value *val_out) {
    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val_out->val1 = value->millivolts / 1000;
        val_out->val2 = (value->millivolts % 1000) * 1000U;
        break;

    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val_out->val1 = value->state_of_charge;
        val_out->val2 = 0;
        break;

    default:
        return -ENOTSUP;
    }

    return 0;
}

uint8_t lithium_ion_mv_to_pct(int16_t bat_mv) {
    // Conductor: anchor 100% at the full-charge resting/loaded voltage on XIAO BLE
    // (~4.1V) rather than the 4.2V CV ceiling, so a completed charge reports 100%
    // instead of capping near 88%. Linear map of the usable 3450-4100mV window.

    if (bat_mv >= 4100) {
        return 100;
    } else if (bat_mv <= 3450) {
        return 0;
    }

    return (bat_mv - 3450) * 2 / 13;
}