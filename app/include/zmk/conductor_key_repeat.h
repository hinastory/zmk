/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* delay_ms / interval_ms of 0 select the Kconfig default; other values are
 * clamped (delay 100-2000, interval 10-200). */
void zmk_key_repeat_set_config(bool enabled, uint32_t delay_ms, uint32_t interval_ms);

/* Returns the effective (clamped) values currently in use. */
void zmk_key_repeat_get_config(bool *enabled, uint32_t *delay_ms, uint32_t *interval_ms);
