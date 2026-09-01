/*
 * Copyright (c) 2026 The Conductor Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Runtime-editable macros.
 *
 * ZMK's &macro is fixed at build time: its bindings come from devicetree, so
 * changing what a macro types normally means editing the keymap and
 * reflashing. This subsystem rewrites a macro's binding list on the running
 * keyboard instead.
 *
 * That is possible only because behavior_macro.c declares its config as
 * `static struct behavior_macro_config` -- every other behaviour uses
 * `static const`. The macro config therefore lives in .data, not flash, and
 * bindings[]/count can be written in place. Nothing in the driver needs to
 * change. If upstream ever adds the missing const, this breaks loudly at
 * compile time (assignment to a const object), which is the failure mode we
 * want rather than a silent no-op.
 *
 * Editable slots are whatever the keymap declares named m_dyn_<n>, padded
 * with &none so bindings[] is allocated large enough to hold a rewritten
 * sequence. Static macros (p1, p2, ...) are listed and readable but rejected
 * for writes -- their arrays are sized to their contents with no room.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_macro.h>
#include <zmk/studio/rpc.h>

ZMK_RPC_SUBSYSTEM(macros)

#define MACROS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(macros, type, __VA_ARGS__)

/* Wire action_type values, shared with the editor (see macros.proto). */
#define MACRO_ACTION_PRESS 1
#define MACRO_ACTION_RELEASE 2
#define MACRO_ACTION_WAIT 3

/* Control behaviours the encoder emits, addressed by device name exactly as
 * behavior_macro.c matches them. */
#define KP_DEV DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))
#define TAP_MODE_DEV DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_tap))
#define PRESS_MODE_DEV DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_press))
#define REL_MODE_DEV DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_release))
#define TAP_TIME_DEV DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_tap_time))
#define WAIT_TIME_DEV DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_wait_time))

struct macro_slot {
    const char *name;
    const struct device *dev;
    uint32_t capacity; /* bindings[] entries the keymap allocated */
    bool editable;     /* named m_dyn_* */
};

#define MACRO_NODE_ENTRY(node)                                                                     \
    {                                                                                              \
        .name = DT_NODE_FULL_NAME(node),                                                           \
        .dev = DEVICE_DT_GET(node),                                                                \
        .capacity = DT_PROP_LEN(node, bindings),                                                   \
        .editable = false,                                                                         \
    },

static struct macro_slot slots[] = {DT_FOREACH_STATUS_OKAY(zmk_behavior_macro, MACRO_NODE_ENTRY)};

#define SLOT_COUNT ARRAY_SIZE(slots)

/*
 * Macros are addressed by behaviour local id, not by position in this table:
 * the editor takes the id from list_all_macros and writes it straight into a
 * key binding, so anything else binds the wrong behaviour.
 *
 * Resolved on every call rather than cached at init. Local ids start out as a
 * crc16 of the device name and are then reassigned from settings while
 * settings_load runs (behavior.c), so an id read during SYS_INIT is not the
 * one list_all_behaviors will report. Any RPC arrives long after that has
 * settled, and the lookup is a handful of string compares.
 */
static zmk_behavior_local_id_t slot_behavior_id(size_t index) {
    return zmk_behavior_get_local_id(slots[index].dev->name);
}

static int slot_index_for_behavior(uint32_t behavior_id) {
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (slot_behavior_id(i) == behavior_id) {
            return (int)i;
        }
    }
    return -1;
}

/* Persisted form: the wire steps, not the encoded bindings. Compact, and it
 * survives a change to the encoder. */
#define MAX_STORED_STEPS 50

/* Scratch for one encode. A step can cost at most a mode change, a timing
 * change and the key itself, so this is comfortably above the worst case for
 * MAX_STORED_STEPS and still small enough for the RPC thread stack. */
#define MAX_ENCODED_BINDINGS 96

struct stored_step {
    uint8_t action;
    uint32_t value;
} __packed;

struct stored_macro {
    uint8_t used;
    uint8_t step_count;
    struct stored_step steps[MAX_STORED_STEPS];
} __packed;

static struct stored_macro stored[SLOT_COUNT];

#define SETTINGS_KEY "macros/studio/slots"
#define NAMES_SETTINGS_KEY "macros/studio/names"

/*
 * User-assigned names, kept in a separate blob from the steps on purpose.
 * Folding them into struct stored_macro would change sizeof(stored), and the
 * loader below rejects a size mismatch -- which would have silently discarded
 * every macro already on the keyboard. Empty means "no name given", and
 * list_all_macros falls back to the devicetree node name.
 *
 * Sized to match zmk.macros.MacroSequence.name (max_size:24 in
 * macros.options), so a name that survives the wire also survives storage.
 */
#define MACRO_NAME_MAX 24

static char stored_names[SLOT_COUNT][MACRO_NAME_MAX];

static int macros_settings_save(void) {
    int ret = settings_save_one(SETTINGS_KEY, &stored, sizeof(stored));
    if (ret < 0) {
        return ret;
    }
    return settings_save_one(NAMES_SETTINGS_KEY, &stored_names, sizeof(stored_names));
}

/* ---- steps -> bindings ------------------------------------------------- */

struct encoder {
    struct zmk_behavior_binding *out;
    uint32_t capacity;
    uint32_t len;
    /* queue_macro starts every run in tap mode with the node's default
     * timings, so track those as the current state and only emit a control
     * binding when something actually differs. */
    const char *mode;
    uint32_t tap_ms;
    uint32_t wait_ms;
    bool overflow;
};

static void emit(struct encoder *e, const char *dev, uint32_t param1) {
    if (e->len >= e->capacity) {
        e->overflow = true;
        return;
    }
    e->out[e->len].behavior_dev = dev;
    e->out[e->len].param1 = param1;
    e->out[e->len].param2 = 0;
    e->len++;
}

static void set_mode(struct encoder *e, const char *mode) {
    if (e->mode == mode) {
        return;
    }
    emit(e, mode, 0);
    e->mode = mode;
}

static void set_wait(struct encoder *e, uint32_t ms) {
    if (e->wait_ms == ms) {
        return;
    }
    emit(e, WAIT_TIME_DEV, ms);
    e->wait_ms = ms;
}

static void set_tap(struct encoder *e, uint32_t ms) {
    if (e->tap_ms == ms) {
        return;
    }
    emit(e, TAP_TIME_DEV, ms);
    e->tap_ms = ms;
}

/* Sum a run of wait steps starting at *i, advancing past them. */
static uint32_t take_waits(const zmk_macros_MacroStep *steps, uint32_t n, uint32_t *i) {
    uint32_t total = 0;
    while (*i < n && steps[*i].action_type == MACRO_ACTION_WAIT) {
        total += steps[*i].value;
        (*i)++;
    }
    return total;
}

/*
 * wait_ms is the delay queued *after* each binding, so a wait step folds into
 * the key that precedes it rather than becoming a binding of its own. A press
 * immediately followed by a release of the same key collapses to one tap
 * binding, which is what keeps a typed string near one binding per character
 * instead of four.
 */
static int encode_steps(const zmk_macros_MacroStep *steps, uint32_t n, struct macro_slot *slot,
                        struct zmk_behavior_binding *out, uint32_t capacity, uint32_t *out_len) {
    uint32_t default_wait = 0, default_tap = 0;
    zmk_behavior_macro_get_defaults(slot->dev, &default_wait, &default_tap);
    struct encoder e = {
        .out = out,
        .capacity = capacity,
        .len = 0,
        .mode = TAP_MODE_DEV,
        .tap_ms = default_tap,
        .wait_ms = default_wait,
    };

    uint32_t i = 0;
    /* A leading wait has nothing to attach to; treat it as the delay before
     * the first key by making it that key's tap/wait spacing instead. */
    uint32_t carried = take_waits(steps, n, &i);

    while (i < n) {
        const zmk_macros_MacroStep *s = &steps[i];

        if (s->action_type == MACRO_ACTION_PRESS) {
            uint32_t j = i + 1;
            uint32_t inner = take_waits(steps, n, &j);
            bool paired = j < n && steps[j].action_type == MACRO_ACTION_RELEASE &&
                          steps[j].value == s->value;

            if (paired) {
                uint32_t after = j + 1;
                uint32_t trailing = take_waits(steps, n, &after);
                set_mode(&e, TAP_MODE_DEV);
                set_tap(&e, inner);
                set_wait(&e, trailing + carried);
                emit(&e, KP_DEV, s->value);
                carried = 0;
                i = after;
                continue;
            }

            /* Unpaired press: a modifier held across later keys. */
            set_mode(&e, PRESS_MODE_DEV);
            set_wait(&e, inner + carried);
            emit(&e, KP_DEV, s->value);
            carried = 0;
            i = j;
            continue;
        }

        if (s->action_type == MACRO_ACTION_RELEASE) {
            uint32_t j = i + 1;
            uint32_t after = take_waits(steps, n, &j);
            set_mode(&e, REL_MODE_DEV);
            set_wait(&e, after + carried);
            emit(&e, KP_DEV, s->value);
            carried = 0;
            i = j;
            continue;
        }

        /* Only waits remain; take_waits above consumed runs, so this is a
         * defensive skip for an unknown action_type. */
        LOG_WRN("macro step %u has unknown action %u, skipped", i, s->action_type);
        i++;
    }

    if (e.overflow) {
        return -ENOSPC;
    }

    *out_len = e.len;
    return 0;
}

/* ---- bindings -> steps ------------------------------------------------- */

static bool dev_is(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

/*
 * Reverse of the above, used for get_macro_data and for showing what the
 * statically declared macros contain. Timings are reported as wait steps so a
 * round trip through the editor preserves them.
 */
static uint32_t decode_bindings(const struct device *macro_dev, zmk_macros_MacroStep *out,
                                uint32_t capacity) {
    const struct zmk_behavior_binding *bindings = NULL;
    int count = zmk_behavior_macro_get_bindings(macro_dev, &bindings);
    if (count < 0) {
        return 0;
    }

    const char *mode = TAP_MODE_DEV;
    uint32_t tap_ms = 0, wait_ms = 0;
    zmk_behavior_macro_get_defaults(macro_dev, &wait_ms, &tap_ms);
    uint32_t len = 0;

    for (int i = 0; i < count && len < capacity; i++) {
        const char *dev = bindings[i].behavior_dev;

        if (dev_is(dev, TAP_MODE_DEV) || dev_is(dev, PRESS_MODE_DEV) || dev_is(dev, REL_MODE_DEV)) {
            mode = dev_is(dev, TAP_MODE_DEV)     ? TAP_MODE_DEV
                   : dev_is(dev, PRESS_MODE_DEV) ? PRESS_MODE_DEV
                                                 : REL_MODE_DEV;
            continue;
        }
        if (dev_is(dev, TAP_TIME_DEV)) {
            tap_ms = bindings[i].param1;
            continue;
        }
        if (dev_is(dev, WAIT_TIME_DEV)) {
            wait_ms = bindings[i].param1;
            continue;
        }
        /* Any other control binding (parameter routing, pause-for-release)
         * has no representation in the flat step list; skip it rather than
         * emit something the editor would misread. */
        if (!dev_is(dev, KP_DEV)) {
            continue;
        }

        uint32_t usage = bindings[i].param1;

        if (mode == TAP_MODE_DEV) {
            if (len < capacity) {
                out[len].action_type = MACRO_ACTION_PRESS;
                out[len].value = usage;
                len++;
            }
            if (tap_ms > 0 && len < capacity) {
                out[len].action_type = MACRO_ACTION_WAIT;
                out[len].value = tap_ms;
                len++;
            }
            if (len < capacity) {
                out[len].action_type = MACRO_ACTION_RELEASE;
                out[len].value = usage;
                len++;
            }
        } else {
            out[len].action_type =
                (mode == PRESS_MODE_DEV) ? MACRO_ACTION_PRESS : MACRO_ACTION_RELEASE;
            out[len].value = usage;
            len++;
        }

        if (wait_ms > 0 && len < capacity) {
            out[len].action_type = MACRO_ACTION_WAIT;
            out[len].value = wait_ms;
            len++;
        }
    }

    return len;
}

/* ---- applying ---------------------------------------------------------- */

static int apply_slot(int index) {
    struct macro_slot *slot = &slots[index];
    struct stored_macro *st = &stored[index];

    zmk_macros_MacroStep steps[MAX_STORED_STEPS];
    for (uint8_t i = 0; i < st->step_count; i++) {
        steps[i].action_type = st->steps[i].action;
        steps[i].value = st->steps[i].value;
    }

    struct zmk_behavior_binding encoded[MAX_ENCODED_BINDINGS];
    uint32_t len = 0;
    int ret = encode_steps(steps, st->step_count, slot, encoded,
                           MIN(slot->capacity, ARRAY_SIZE(encoded)), &len);
    if (ret < 0) {
        return ret;
    }

    return zmk_behavior_macro_set_bindings(slot->dev, encoded, len);
}

/* ---- RPC handlers ------------------------------------------------------ */

zmk_studio_Response list_all_macros(const zmk_studio_Request *req) {
    zmk_macros_ListAllMacrosResponse resp = zmk_macros_ListAllMacrosResponse_init_zero;
    resp.max_macros = SLOT_COUNT;
    resp.macros_count = 0;

    for (size_t i = 0; i < SLOT_COUNT && i < ARRAY_SIZE(resp.macros); i++) {
        const struct zmk_behavior_binding *bindings = NULL;
        int binding_count = zmk_behavior_macro_get_bindings(slots[i].dev, &bindings);
        zmk_macros_MacroSummary *m = &resp.macros[resp.macros_count++];
        m->id = slot_behavior_id(i);
        const char *label = stored_names[i][0] ? stored_names[i] : slots[i].name;
        strncpy(m->name, label, sizeof(m->name) - 1);
        m->name[sizeof(m->name) - 1] = '\0';
        m->step_count = slots[i].editable ? stored[i].step_count : MAX(binding_count, 0);
    }

    return MACROS_RESPONSE(list_all_macros, resp);
}

zmk_studio_Response get_macro_data(const zmk_studio_Request *req) {
    uint32_t id = req->subsystem.macros.request_type.get_macro_data.macro_id;
    zmk_macros_GetMacroDataResponse resp = zmk_macros_GetMacroDataResponse_init_zero;
    int slot = slot_index_for_behavior(id);

    if (slot < 0) {
        resp.which_result = zmk_macros_GetMacroDataResponse_err_tag;
        resp.result.err = zmk_macros_GetMacroDataErrorCode_GET_MACRO_DATA_ERR_INVALID_ID;
        return MACROS_RESPONSE(get_macro_data, resp);
    }

    resp.which_result = zmk_macros_GetMacroDataResponse_macro_tag;
    resp.result.macro.id = id;
    const char *label = stored_names[slot][0] ? stored_names[slot] : slots[slot].name;
    strncpy(resp.result.macro.name, label, sizeof(resp.result.macro.name) - 1);
    resp.result.macro.steps_count = decode_bindings(slots[slot].dev, resp.result.macro.steps,
                                                    ARRAY_SIZE(resp.result.macro.steps));

    return MACROS_RESPONSE(get_macro_data, resp);
}

zmk_studio_Response set_macro(const zmk_studio_Request *req) {
    const zmk_macros_MacroSequence *macro = &req->subsystem.macros.request_type.set_macro.macro;
    int id = slot_index_for_behavior(macro->id);

    if (id < 0) {
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_INVALID_ID);
    }
    if (!slots[id].editable) {
        LOG_WRN("macro %s is declared in the keymap, not an editable slot", slots[id].name);
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_INVALID_ID);
    }
    if (macro->steps_count > MAX_STORED_STEPS) {
        return MACROS_RESPONSE(set_macro,
                               zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_TOO_MANY_STEPS);
    }

    /* Encode before committing anything, so a sequence that will not fit
     * leaves the slot as it was rather than half-written. */
    struct zmk_behavior_binding probe[MAX_ENCODED_BINDINGS];
    uint32_t len = 0;
    int ret = encode_steps(macro->steps, macro->steps_count, &slots[id], probe,
                           MIN(slots[id].capacity, ARRAY_SIZE(probe)), &len);
    if (ret == -ENOSPC) {
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_NO_SPACE);
    }
    if (ret < 0) {
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_GENERIC);
    }

    stored[id].used = 1;
    stored[id].step_count = macro->steps_count;
    strncpy(stored_names[id], macro->name, MACRO_NAME_MAX - 1);
    stored_names[id][MACRO_NAME_MAX - 1] = '\0';
    for (uint32_t i = 0; i < macro->steps_count; i++) {
        stored[id].steps[i].action = macro->steps[i].action_type;
        stored[id].steps[i].value = macro->steps[i].value;
    }

    ret = apply_slot(id);
    if (ret < 0) {
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_GENERIC);
    }

    ret = macros_settings_save();
    if (ret < 0) {
        LOG_ERR("failed to persist macro %s: %d", slots[id].name, ret);
        return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_ERR_GENERIC);
    }

    LOG_DBG("macro %s: %u steps -> %u bindings", slots[id].name, macro->steps_count, len);
    return MACROS_RESPONSE(set_macro, zmk_macros_SetMacroResponse_SET_MACRO_RESP_OK);
}

ZMK_RPC_SUBSYSTEM_HANDLER(macros, list_all_macros, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, get_macro_data, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(macros, set_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return -ENOTSUP; }
ZMK_RPC_EVENT_MAPPER(macros, event_mapper);

/* ---- settings ---------------------------------------------------------- */

static int macros_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (settings_name_steq(name, "slots", NULL)) {
        if (len != sizeof(stored)) {
            LOG_WRN("stored macros are %zu bytes, expected %zu -- ignoring", len, sizeof(stored));
            return 0;
        }
        int rc = read_cb(cb_arg, &stored, sizeof(stored));
        return rc >= 0 ? 0 : rc;
    }
    if (settings_name_steq(name, "names", NULL)) {
        /* Absent on a keyboard written by an older build: leave the names
         * empty and every macro keeps reporting its node name. */
        if (len != sizeof(stored_names)) {
            LOG_WRN("stored macro names are %zu bytes, expected %zu -- ignoring", len,
                    sizeof(stored_names));
            return 0;
        }
        int rc = read_cb(cb_arg, &stored_names, sizeof(stored_names));
        return rc >= 0 ? 0 : rc;
    }
    return -ENOENT;
}

static int macros_settings_commit(void) {
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (!slots[i].editable || !stored[i].used) {
            continue;
        }
        int ret = apply_slot(i);
        if (ret < 0) {
            LOG_ERR("could not restore macro %zu (%s): %d", i, slots[i].name, ret);
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(macros_studio, "macros/studio", NULL, macros_settings_set,
                               macros_settings_commit, NULL);

static int macros_settings_reset(void) {
    memset(&stored, 0, sizeof(stored));
    memset(&stored_names, 0, sizeof(stored_names));
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (slots[i].editable) {
            apply_slot(i);
        }
    }
    return macros_settings_save();
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(macros, macros_settings_reset);

/* ---- init -------------------------------------------------------------- */

static int macros_subsystem_init(void) {
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        slots[i].editable = strncmp(slots[i].name, "m_dyn_", 6) == 0;
        LOG_DBG("macro slot %zu: %s (device %s), %u bindings, %s", i, slots[i].name,
                slots[i].dev->name, slots[i].capacity,
                slots[i].editable ? "editable" : "from keymap");
    }
    return 0;
}

SYS_INIT(macros_subsystem_init, APPLICATION, 98);
