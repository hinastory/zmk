/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/behavior.h>
#include <zmk/conductor_gesture.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_CONDUCTOR_GESTURE_PROFILE)

#define GESTURE_DIR_COUNT 4
#define GST_BLOB_VERSION 1
#define GST_SETTINGS_KEY "cond/gst/v1"

/* Runtime override table. behavior_dev == NULL means "unset" (fall back to the
 * gesture layer default). behavior_dev is a resolved, ready-to-tap name pointer
 * so the input-thread read is a plain struct copy with no behavior lookup. */
struct gesture_slot {
    const char *behavior_dev;
    zmk_behavior_local_id_t local_id;
    uint32_t param1;
    uint32_t param2;
};

static struct gesture_slot table[ZMK_ENDPOINT_COUNT][GESTURE_DIR_COUNT];
static bool gst_enabled;
/* Guards every read/write of `table` and `gst_enabled`. The input thread copies
 * a single binding out under this lock; the RPC/commit threads swap the whole
 * table under it. A spinlock (not a mutex) keeps the input path non-blocking. */
static struct k_spinlock lock;

/* Persisted layout. The leading version / endpoint_count / dir_count let a
 * future firmware whose endpoint indexing differs detect and discard stale
 * blobs instead of mis-indexing them. */
struct gst_slot_blob {
    uint8_t present;
    uint16_t local_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

struct gst_blob {
    uint8_t version;
    uint8_t enabled;
    uint8_t endpoint_count;
    uint8_t dir_count;
    struct gst_slot_blob slots[ZMK_ENDPOINT_COUNT][GESTURE_DIR_COUNT];
} __packed;

static int selected_endpoint_index(void) {
    int idx = zmk_endpoint_instance_to_index(zmk_endpoint_get_selected());
    if (idx < 0) {
        idx = 0;
    } else if (idx >= ZMK_ENDPOINT_COUNT) {
        idx = ZMK_ENDPOINT_COUNT - 1;
    }
    return idx;
}

/* Resolve behavior names (a registry scan — must NOT run under the spinlock),
 * then swap the resolved rows and enable flag into the live table atomically. */
static void apply_blob(const struct gst_blob *blob) {
    struct gesture_slot resolved[ZMK_ENDPOINT_COUNT][GESTURE_DIR_COUNT];
    memset(resolved, 0, sizeof(resolved));

    for (int ep = 0; ep < ZMK_ENDPOINT_COUNT; ep++) {
        for (int d = 0; d < GESTURE_DIR_COUNT; d++) {
            const struct gst_slot_blob *sb = &blob->slots[ep][d];
            if (!sb->present) {
                continue;
            }
            const char *name = zmk_behavior_find_behavior_name_from_local_id(sb->local_id);
            if (!name) {
                LOG_WRN("gesture [%d][%d]: unknown behavior id %u — leaving unset", ep, d,
                        sb->local_id);
                continue;
            }
            resolved[ep][d].behavior_dev = name;
            resolved[ep][d].local_id = sb->local_id;
            resolved[ep][d].param1 = sb->param1;
            resolved[ep][d].param2 = sb->param2;
        }
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    memcpy(table, resolved, sizeof(table));
    gst_enabled = blob->enabled ? true : false;
    k_spin_unlock(&lock, key);
}

static void build_blob(struct gst_blob *blob, bool enabled, const struct conductor_gesture_dir *dirs,
                       uint32_t endpoint_count) {
    memset(blob, 0, sizeof(*blob));
    blob->version = GST_BLOB_VERSION;
    blob->enabled = enabled ? 1 : 0;
    blob->endpoint_count = ZMK_ENDPOINT_COUNT;
    blob->dir_count = GESTURE_DIR_COUNT;

    size_t rows = MIN(endpoint_count, (uint32_t)ZMK_ENDPOINT_COUNT);
    for (size_t ep = 0; ep < rows; ep++) {
        for (int d = 0; d < GESTURE_DIR_COUNT; d++) {
            const struct conductor_gesture_dir *src = &dirs[ep * GESTURE_DIR_COUNT + d];
            if (!src->present) {
                continue;
            }
            blob->slots[ep][d].present = 1;
            blob->slots[ep][d].local_id = src->local_id;
            blob->slots[ep][d].param1 = src->param1;
            blob->slots[ep][d].param2 = src->param2;
        }
    }
}

static void persist_blob(const struct gst_blob *blob) {
    int rc = settings_save_one(GST_SETTINGS_KEY, blob, sizeof(*blob));
    if (rc) {
        LOG_ERR("Failed to persist gesture config: %d", rc);
    }
}

bool conductor_gesture_get_binding(int direction, struct zmk_behavior_binding *out) {
    if (direction < 0 || direction >= GESTURE_DIR_COUNT) {
        return false;
    }
    int ep = selected_endpoint_index();
    bool found = false;

    k_spinlock_key_t key = k_spin_lock(&lock);
    if (gst_enabled) {
        const struct gesture_slot *s = &table[ep][direction];
        if (s->behavior_dev != NULL) {
            memset(out, 0, sizeof(*out));
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            out->local_id = s->local_id;
#endif
            out->behavior_dev = s->behavior_dev;
            out->param1 = s->param1;
            out->param2 = s->param2;
            found = true;
        }
    }
    k_spin_unlock(&lock, key);
    return found;
}

void conductor_gesture_snapshot(bool *enabled, uint32_t *active_endpoint, uint32_t *endpoint_count,
                                struct conductor_gesture_dir *dirs_out, size_t dirs_cap) {
    if (endpoint_count) {
        *endpoint_count = ZMK_ENDPOINT_COUNT;
    }
    if (active_endpoint) {
        *active_endpoint = (uint32_t)selected_endpoint_index();
    }

    struct gesture_slot snap[ZMK_ENDPOINT_COUNT][GESTURE_DIR_COUNT];
    bool en;
    k_spinlock_key_t key = k_spin_lock(&lock);
    en = gst_enabled;
    memcpy(snap, table, sizeof(snap));
    k_spin_unlock(&lock, key);

    if (enabled) {
        *enabled = en;
    }
    if (!dirs_out) {
        return;
    }
    size_t idx = 0;
    for (int ep = 0; ep < ZMK_ENDPOINT_COUNT; ep++) {
        for (int d = 0; d < GESTURE_DIR_COUNT; d++) {
            if (idx >= dirs_cap) {
                return;
            }
            struct conductor_gesture_dir *o = &dirs_out[idx++];
            o->present = snap[ep][d].behavior_dev != NULL;
            o->local_id = snap[ep][d].local_id;
            o->param1 = snap[ep][d].param1;
            o->param2 = snap[ep][d].param2;
        }
    }
}

void conductor_gesture_set_config(bool enabled, const struct conductor_gesture_dir *dirs,
                                  uint32_t endpoint_count) {
    struct gst_blob blob;
    build_blob(&blob, enabled, dirs, endpoint_count);
    apply_blob(&blob);
    persist_blob(&blob);
    LOG_DBG("gesture config set: enabled=%d endpoint_count=%u", enabled, endpoint_count);
}

int conductor_gesture_reset(void) {
    struct gst_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = GST_BLOB_VERSION;
    blob.endpoint_count = ZMK_ENDPOINT_COUNT;
    blob.dir_count = GESTURE_DIR_COUNT;
    apply_blob(&blob);
    int rc = settings_delete(GST_SETTINGS_KEY);
    if (rc == -ENOENT) {
        rc = 0; /* nothing persisted is not an error */
    }
    if (rc) {
        LOG_WRN("Failed to delete gesture config: %d", rc);
    }
    return rc;
}

/* Settings: read the raw blob in h_set (behavior name registry may not be ready
 * yet), resolve + apply it in h_commit (after settings_load completes), exactly
 * like the combo subsystem applies stored combos in its commit. */
static struct gst_blob loaded_blob;
static bool loaded_valid;

static int gesture_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "v1", &next) && !next) {
        loaded_valid = false;
        if (len != sizeof(loaded_blob)) {
            LOG_WRN("gesture blob size mismatch (%u != %u) — discarding", (unsigned)len,
                    (unsigned)sizeof(loaded_blob));
            return 0;
        }
        int rc = read_cb(cb_arg, &loaded_blob, sizeof(loaded_blob));
        if (rc < 0) {
            return rc;
        }
        if (loaded_blob.version != GST_BLOB_VERSION ||
            loaded_blob.endpoint_count != ZMK_ENDPOINT_COUNT ||
            loaded_blob.dir_count != GESTURE_DIR_COUNT) {
            LOG_WRN("gesture blob layout mismatch — discarding");
            return 0;
        }
        loaded_valid = true;
        return 0;
    }
    return -ENOENT;
}

static int gesture_settings_commit(void) {
    if (loaded_valid) {
        apply_blob(&loaded_blob);
        LOG_INF("Applied stored gesture config (enabled=%d)", loaded_blob.enabled);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(conductor_gesture_settings, "cond/gst", NULL, gesture_settings_set,
                               gesture_settings_commit, NULL);

#endif /* IS_ENABLED(CONFIG_CONDUCTOR_GESTURE_PROFILE) */
