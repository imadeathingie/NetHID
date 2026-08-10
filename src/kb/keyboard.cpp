/*
 * keyboard.cpp — the scan pipeline and the feature dispatch chain.
 *
 * This file is deliberately small. It knows how to turn matrix edges into
 * keyrecord_t, how to walk the feature chain, and how to apply a plain
 * keycode. Every behaviour beyond that lives in src/kb/features/.
 */

#include "kb/kb.h"
#include "kb/matrix.h"
#include "kb/debounce.h"
#include "kb/keystate.h"
#include "kb/features.h"
#if KB_FEATURE_LAYERS
#include "kb/layers.h"
#endif
#include "kb/bootmagic.h"
#if SPLIT_ENABLE
#include "split/split.h"
#endif
#include "kb/encoder.h"
#include "oled/kb_status.h"
#include "pico/stdlib.h"
#if KB_FEATURE_DYNAMIC_KEYMAP
#include "pico/multicore.h"
#endif
#include <string.h>
#include <stdio.h>
#if ENABLE_SETTINGS
#include "settings.h"
#endif

static matrix_row_t raw[MATRIX_ROWS];
static matrix_row_t cooked[MATRIX_ROWS];
static matrix_row_t applied[MATRIX_ROWS];

/* Keycode each position resolved to at press time. A key must release the
 * same keycode it pressed, even if the layer changed underneath it. */
static kb_keycode_t held_kc[MATRIX_ROWS][MATRIX_COLS];

/* Reference-counted weak modifiers, so two overlapping LSFT(..) keys don't
 * cancel each other's shift on the first release. */
static uint8_t weak_refs[8];

static const kb_feature_t features[] = {
#define KB_FEATURE(name) { kb_##name##_init, kb_##name##_process, kb_##name##_task, #name },
    KB_FEATURE_LIST
#undef KB_FEATURE
};
static const size_t feature_count = sizeof(features) / sizeof(features[0]);

/* ── Keycode lookup ───────────────────────────────────────────────────── */

kb_keycode_t kb_keycode_at(keypos_t pos) {
    if (pos.row >= MATRIX_ROWS || pos.col >= MATRIX_COLS) return KC_NO;
#if KB_FEATURE_LAYERS
    return kb_layer_lookup(pos);
#else
    return kb_keymap_at(0, pos.row, pos.col);
#endif
}

#if NUM_ENCODERS > 0
/* One accessor so the compiled array and the runtime store cannot diverge. */
static inline kb_keycode_t kb_enc_lookup(uint8_t layer, uint8_t index, uint8_t action) {
#if KB_FEATURE_DYNAMIC_KEYMAP
    return kb_encoder_at(layer, index, action);
#else
    return (layer < encoder_map_layers) ? encoder_map[layer][index][action] : KC_TRNS;
#endif
}

kb_keycode_t kb_encoder_keycode(uint8_t index, int action) {
    if (index >= NUM_ENCODERS || action < 0 || action > 2) return KC_NO;
#if KB_FEATURE_LAYERS
    /* Walk the layer stack the same way a key does, honouring KC_TRNS, so a
     * layer that says nothing about a knob leaves it doing what the layer below
     * says rather than going dead. */
    for (int l = 31; l >= 0; l--) {
        if (!kb_layer_is_on((uint8_t)l)) continue;
        if (l >= kb_keymap_layers()) continue;
        kb_keycode_t kc = kb_enc_lookup((uint8_t)l, index, (uint8_t)action);
        if (kc != KC_TRNS) return kc;
    }
    return kb_enc_lookup(0, index, (uint8_t)action);
#else
    return kb_enc_lookup(0, index, (uint8_t)action);
#endif
}
#endif

/*
 * Words per minute over a rolling 10-second window, five characters to a word.
 *
 * A decaying count rather than a ring buffer of timestamps: it is one add and
 * one multiply per second, it cannot be made to allocate, and nobody has ever
 * cared whether their WPM readout was exactly right.
 */
static uint16_t wpm_chars;
static uint32_t wpm_at;

static void wpm_tick(uint32_t now) {
    while ((now - wpm_at) >= 1000) {
        wpm_chars = (uint16_t)(wpm_chars * 9 / 10);   /* ~10s time constant */
        wpm_at += 1000;
    }
}

uint8_t kb_wpm(void) {
    uint32_t w = ((uint32_t)wpm_chars * 60u) / (10u * 5u);
    return (uint8_t)(w > 255 ? 255 : w);
}

/* ── Applying a resolved keycode ──────────────────────────────────────── */

static void weak_mods_add(uint8_t mods) {
    for (int b = 0; b < 8; b++) if (mods & (1u << b)) weak_refs[b]++;
    uint8_t m = 0;
    for (int b = 0; b < 8; b++) if (weak_refs[b]) m |= (uint8_t)(1u << b);
    keystate_set_weak_mods(KB_SRC_MATRIX, m);
}

static void weak_mods_del(uint8_t mods) {
    for (int b = 0; b < 8; b++) if ((mods & (1u << b)) && weak_refs[b]) weak_refs[b]--;
    uint8_t m = 0;
    for (int b = 0; b < 8; b++) if (weak_refs[b]) m |= (uint8_t)(1u << b);
    keystate_set_weak_mods(KB_SRC_MATRIX, m);
}

void kb_register(kb_keycode_t kc) {
    /* Count only real characters: modifiers and layer keys are not typing. */
    if (kc_is_basic(kc) && !kc_is_mod(kc) && kc >= KEY_A && wpm_chars < 4000) wpm_chars++;
    if (kc_is_basic(kc)) {
        keystate_press(KB_SRC_MATRIX, kc_basic_of(kc));
    } else if (kc_in(kc, QK_MODS, QK_MODS_MAX)) {
        weak_mods_add(kc_unpack_mods(kc));
        keystate_press(KB_SRC_MATRIX, kc_basic_of(kc));
    }
}

void kb_unregister(kb_keycode_t kc) {
    if (kc_is_basic(kc)) {
        keystate_release(KB_SRC_MATRIX, kc_basic_of(kc));
    } else if (kc_in(kc, QK_MODS, QK_MODS_MAX)) {
        keystate_release(KB_SRC_MATRIX, kc_basic_of(kc));
        weak_mods_del(kc_unpack_mods(kc));
    }
}

/* ── The chain ────────────────────────────────────────────────────────── */

static void dispatch(keyrecord_t *rec) {
    for (size_t i = 0; i < feature_count; i++) {
        if (!features[i].process(rec)) return;   /* swallowed */
    }
    /* Default handler: anything the features didn't claim. */
#if KB_FEATURE_BOOTMAGIC
    if (rec->keycode == QK_BOOT) {
        if (rec->event.pressed) kb_bootloader_request();
        return;
    }
#endif
    if (rec->event.pressed) kb_register(rec->keycode);
    else                    kb_unregister(rec->keycode);
}

void kb_dispatch_after(bool (*self)(keyrecord_t *), keyrecord_t *rec) {
    size_t start = 0;
    for (size_t i = 0; i < feature_count; i++)
        if (features[i].process == self) { start = i + 1; break; }
    for (size_t i = start; i < feature_count; i++)
        if (!features[i].process(rec)) return;
#if KB_FEATURE_BOOTMAGIC
    if (rec->keycode == QK_BOOT) {
        if (rec->event.pressed) kb_bootloader_request();
        return;
    }
#endif
    if (rec->event.pressed) kb_register(rec->keycode);
    else                    kb_unregister(rec->keycode);
}

void kb_send_keycode(kb_keycode_t kc, bool pressed) {
    keyrecord_t rec = {};
    rec.event.key     = KEYPOS_NONE;
    rec.event.pressed = pressed;
    rec.event.time    = to_ms_since_boot(get_absolute_time());
    rec.keycode       = kc;
    dispatch(&rec);
}

/* ── Init / task ──────────────────────────────────────────────────────── */

void kb_init(void) {
    memset(raw, 0, sizeof(raw));
    memset(cooked, 0, sizeof(cooked));
    memset(applied, 0, sizeof(applied));
    memset(held_kc, 0, sizeof(held_kc));
    memset(weak_refs, 0, sizeof(weak_refs));

    matrix_init();
    encoder_init();
#if SPLIT_ENABLE
    split_primary_init();
#endif
    debounce_init();
    keystate_init();
#if KB_FEATURE_DYNAMIC_KEYMAP
    kb_keymap_store_init();
    /* Opt this core into being parked in RAM while core 0 writes flash. Without
     * it, the erase kills XIP under our feet and this core faults.
     *
     * Drain first: multicore_launch_core1() used the inter-core FIFO for its
     * handshake, and the lockout handler triggers on FIFO traffic. A leftover
     * word would park this core the moment the IRQ is enabled — which looks
     * exactly like a dead matrix. Nothing else in NetHID uses the FIFO; if you
     * add something that does, it will fight this. */
    multicore_fifo_drain();
    multicore_fifo_clear_irq();
    multicore_lockout_victim_init();
#endif
    for (size_t i = 0; i < feature_count; i++) features[i].init();
}

void kb_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    wpm_tick(now);

    matrix_scan(raw);
#if SPLIT_ENABLE
    // Poll the link, then fold the other half's rows into the same raw array
    // before debounce. Debouncing the combined matrix means one implementation
    // and one set of timings for both halves; the alternative is each half
    // debouncing its own and the two ending up with measurably different
    // latency, only one of which is tunable.
    split_primary_task();
    split_primary_rows(raw);   /* fills every row not scanned locally */
#endif
    debounce_run(raw, cooked, now);

    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        matrix_row_t diff = cooked[r] ^ applied[r];
        if (!diff) continue;
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            matrix_row_t bit = (matrix_row_t)1u << c;
            if (!(diff & bit)) continue;
            applied[r] ^= bit;

            keyrecord_t rec = {};
            rec.event.key     = (keypos_t){ r, c };
            rec.event.pressed = (cooked[r] & bit) != 0;
            rec.event.time    = now;

            // Raw position log, independent of the keymap: this fires even for
            // positions mapped to KC_NO, so it also shows keys you have not
            // accounted for and any ghosting from missing diodes. Runtime
            // toggleable, because working out a matrix is exactly when you do
            // not want to be reflashing.
#if ENABLE_SETTINGS
            if (settings()->debug_matrix)
#elif KB_DEBUG_MATRIX
            if (true)
#else
            if (false)
#endif
                printf("[matrix] r%u c%u %s\n", r, c, (cooked[r] & bit) ? "down" : "up");
            if (rec.event.pressed) {
                rec.keycode = kb_keycode_at(rec.event.key);
                held_kc[r][c] = rec.keycode;
            } else {
                rec.keycode = held_kc[r][c];
                held_kc[r][c] = KC_NO;
            }
            dispatch(&rec);
        }
    }

#if NUM_ENCODERS > 0
    /* Encoders resolve through the SAME feature chain as keys, so a rotation can
     * carry a mod-tap, fire a macro or be remapped from the web editor without
     * any of those features knowing an encoder exists.
     *
     * A detent is momentary — there is no held state for a rotation — so it is
     * emitted as a press and released on the following scan. Doing both in one
     * scan would collapse into a single composed report and the host would never
     * see the key at all, which is the same trap tapping.cpp works around. */
    encoder_task();
    static kb_keycode_t enc_tap;
    if (enc_tap) { kb_send_keycode(enc_tap, false); enc_tap = KC_NO; }
    else {
        uint8_t idx; enc_action_t act; bool pressed;
        if (encoder_next(&idx, &act, &pressed)) {
            kb_keycode_t kc = kb_encoder_keycode(idx, act);
            if (kc != KC_NO) {
                if (act == ENC_PRESS) {
                    kb_send_keycode(kc, pressed);       /* a real button: held */
                } else {
                    kb_send_keycode(kc, true);
                    enc_tap = kc;                       /* released next scan */
                }
            }
        }
    }
#endif

#if OLED_ENABLE
    /* Publish a snapshot for the display, which renders on core 0. Scalars only
     * and no lock — see the note in kb_status.cpp. */
    {
        kb_status_t st = {};
#if KB_FEATURE_LAYERS
        st.layer = kb_layer_highest();
#endif
        st.mods  = keystate_get_mods(KB_SRC_MATRIX);
        st.flags = (uint8_t)(tud_mounted() ? KB_ST_USB : 0);
        st.wpm   = kb_wpm();
        kb_status_set(&st);
    }
#endif

    /* Timeouts: tapping terms, one-shot expiry, combo windows. */
    for (size_t i = 0; i < feature_count; i++) features[i].task();
}
