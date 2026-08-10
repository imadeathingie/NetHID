/*
 * combo.cpp — chords.
 *
 * Runs first in the chain because it has to see presses before anything
 * defers or rewrites them. A key that participates in some combo is held
 * back for up to COMBO_TERM_MS; if the rest of the chord arrives the combo
 * fires, otherwise the held keys are replayed into the chain from just after
 * this feature, so a deferred layer key or mod-tap still behaves correctly.
 *
 * Cost of the deferral is COMBO_TERM_MS of latency on exactly those keys
 * that appear in a combo list. Keep the lists small.
 */

#include "pico/stdlib.h"
#include "kb/combo.h"
#include "kb/features.h"

#ifndef COMBO_BUF
#define COMBO_BUF 6
#endif

static keyrecord_t deferred[COMBO_BUF];
static uint8_t     n_deferred;
static uint32_t    first_at;

static const kb_combo_t *fired;
static keypos_t          fired_keys[COMBO_MAX_KEYS];
static uint8_t           fired_n;

static bool in_any_combo(kb_keycode_t kc) {
    for (uint16_t i = 0; i < kb_combo_count; i++)
        for (const kb_keycode_t *k = kb_combos[i].keys; *k != KC_NO; k++)
            if (*k == kc) return true;
    return false;
}

static bool deferred_has(kb_keycode_t kc) {
    for (uint8_t i = 0; i < n_deferred; i++) if (deferred[i].keycode == kc) return true;
    return false;
}

/* Does the deferred set contain every key of combo `c`? */
static bool combo_complete(const kb_combo_t *c, uint8_t *len_out) {
    uint8_t len = 0;
    for (const kb_keycode_t *k = c->keys; *k != KC_NO; k++) {
        if (!deferred_has(*k)) return false;
        len++;
    }
    *len_out = len;
    return len > 0;
}

static void flush(void) {
    uint8_t n = n_deferred;
    n_deferred = 0;                     /* clear first: replay re-enters us */
    for (uint8_t i = 0; i < n; i++) kb_dispatch_after(kb_combo_process, &deferred[i]);
}

static void fire(const kb_combo_t *c) {
    fired = c;
    fired_n = 0;
    for (uint8_t i = 0; i < n_deferred && fired_n < COMBO_MAX_KEYS; i++)
        fired_keys[fired_n++] = deferred[i].event.key;
    n_deferred = 0;
    kb_register(c->action);
}

void kb_combo_init(void) { n_deferred = 0; fired = nullptr; fired_n = 0; }

bool kb_combo_process(keyrecord_t *rec) {
    /* Releases of the keys that produced a live combo belong to the combo. */
    if (fired && !rec->event.pressed) {
        for (uint8_t i = 0; i < fired_n; i++) {
            if (fired_keys[i].row == rec->event.key.row &&
                fired_keys[i].col == rec->event.key.col) {
                kb_unregister(fired->action);
                fired = nullptr;
                fired_n = 0;
                return false;
            }
        }
    }

    if (kb_combo_count == 0) return true;

    if (rec->event.pressed && in_any_combo(rec->keycode) && n_deferred < COMBO_BUF) {
        if (n_deferred == 0) first_at = rec->event.time;
        deferred[n_deferred++] = *rec;

        for (uint16_t i = 0; i < kb_combo_count; i++) {
            uint8_t len;
            if (combo_complete(&kb_combos[i], &len) && len == n_deferred) {
                fire(&kb_combos[i]);
                return false;
            }
        }
        return false;   /* held back for now */
    }

    /* Anything else ends the window: replay first, then let this event run. */
    if (n_deferred) flush();
    return true;
}

void kb_combo_task(void) {
    if (!n_deferred) return;
    if (to_ms_since_boot(get_absolute_time()) - first_at > COMBO_TERM_MS) flush();
}
