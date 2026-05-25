/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Hardware watchdog fed from the system workqueue.
 * If the workqueue hangs (e.g. BLE controller deadlock),
 * the feed stops and the WDT triggers an automatic MCU reset.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WDT_NODE DT_NODELABEL(wdt0)

static const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);
static int wdt_channel_id;
static struct k_work_delayable wdt_feed_work;

static void wdt_feed_handler(struct k_work *work) {
    wdt_feed(wdt, wdt_channel_id);
    k_work_schedule(&wdt_feed_work, K_MSEC(CONFIG_ZMK_WATCHDOG_FEED_INTERVAL_MS));
}

static int zmk_watchdog_init(void) {
    if (!device_is_ready(wdt)) {
        LOG_ERR("WDT device not ready");
        return -ENODEV;
    }

    struct wdt_timeout_cfg cfg = {
        .window = {
            .min = 0,
            .max = CONFIG_ZMK_WATCHDOG_TIMEOUT_MS,
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt, &cfg);
    if (wdt_channel_id < 0) {
        LOG_ERR("WDT install failed: %d", wdt_channel_id);
        return wdt_channel_id;
    }

    int ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        LOG_ERR("WDT setup failed: %d", ret);
        return ret;
    }

    /* Feed once immediately, then schedule periodic feeds on system workqueue */
    wdt_feed(wdt, wdt_channel_id);
    k_work_init_delayable(&wdt_feed_work, wdt_feed_handler);
    k_work_schedule(&wdt_feed_work, K_MSEC(CONFIG_ZMK_WATCHDOG_FEED_INTERVAL_MS));

    LOG_INF("Watchdog: %dms timeout, %dms feed interval",
            CONFIG_ZMK_WATCHDOG_TIMEOUT_MS, CONFIG_ZMK_WATCHDOG_FEED_INTERVAL_MS);
    return 0;
}

SYS_INIT(zmk_watchdog_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
