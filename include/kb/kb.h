/*
 * kb/kb.h — physical keyboard layer: public API and core types.
 *
 * Pipeline, once per scan, on core 1 (next to hid_task()):
 *
 *   matrix_scan()  raw GPIO state
 *        │
 *   debounce()     stable state
 *        │
 *   keyboard.cpp   diff → keyevent_t → resolve keycode → feature chain
 *        │
 *   keystate.cpp   held-key bitmap for KB_SRC_MATRIX
 *        │
 *   hid.cpp        composes MATRIX ⊕ NET into one report and sends it
 *
 * Nothing here allocates; everything is fixed-size and sized from the
 * selected keyboard's keyboard.h.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"        /* from keyboards/<KEYBOARD>/ — provides MATRIX_* */
#include "kb/keycodes.h"
#include "kb/encoder.h"

#ifndef MATRIX_ROWS
#error "keyboard.h did not define MATRIX_ROWS — is -DKEYBOARD=... set?"
#endif

/* Log every debounced edge to the UART as "[matrix] rN cM down/up". Build with
 * -DKB_DEBUG_MATRIX=1 when you are working out how a board is wired. */
#ifndef KB_DEBUG_MATRIX
#define KB_DEBUG_MATRIX 0
#endif

/* Position in the physical matrix. row==0xFF means "not a matrix key"
 * (used by combos and macros that synthesise events). */
typedef struct {
    uint8_t row;
    uint8_t col;
} keypos_t;

#define KEYPOS_NONE ((keypos_t){0xFF, 0xFF})

typedef struct {
    keypos_t key;
    bool     pressed;
    uint32_t time;        /* ms since boot, from to_ms_since_boot() */
} keyevent_t;

/* A key event plus everything the feature chain needs to reason about it. */
typedef struct {
    keyevent_t   event;
    kb_keycode_t keycode;   /* resolved at press; replayed verbatim on release */
    uint8_t      tap_count; /* set by the tapping feature; 0 = not a tap */
} keyrecord_t;

/* The keymap, supplied by keyboards/<KEYBOARD>/keymaps/<KEYMAP>/keymap.cpp.
 *
 * Declared here rather than in the .cpp files that use it for a C++ reason
 * that will otherwise cost you an afternoon: at namespace scope a `const`
 * object has INTERNAL linkage by default. Without this extern declaration
 * visible ahead of the definition, `const kb_keycode_t keymaps[][..]` in the
 * keymap is private to that translation unit and the link fails. Every keymap
 * includes kb/kb.h first, which is what makes it external. */
extern const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS];
extern const uint8_t      keymap_layer_count;

#ifndef KB_FEATURE_DYNAMIC_KEYMAP
#define KB_FEATURE_DYNAMIC_KEYMAP 0
#endif

/* Everything reads the keymap through these two, never through `keymaps`
 * directly. With KB_FEATURE_DYNAMIC_KEYMAP they hit a mutable RAM copy that
 * the web UI can edit; without it they inline to a flash read of the compiled
 * array and cost nothing. */
#if KB_FEATURE_DYNAMIC_KEYMAP
#include "kb/keymap_store.h"
#else
static inline uint16_t kb_keymap_at(uint8_t layer, uint8_t row, uint8_t col) {
    return keymaps[layer][row][col];
}
static inline uint8_t kb_keymap_layers(void) { return keymap_layer_count; }
#endif

#if NUM_ENCODERS > 0
/* Supplied by the keymap: [layer][encoder][CCW, CW, press].
 * Looked up through the live layer stack, so a knob can mean different things
 * on different layers exactly as a key does. */
extern const kb_keycode_t encoder_map[][NUM_ENCODERS][3];
extern const uint8_t      encoder_map_layers;
kb_keycode_t kb_encoder_keycode(uint8_t index, int action);
#endif

/* Rolling words-per-minute estimate, for the status display. */
uint8_t kb_wpm(void);

/* ── Core entry points ────────────────────────────────────────────────── */
void kb_init(void);   /* call once, on core 1, before the loop */
void kb_task(void);   /* call every core-1 iteration */

/* Resolve a matrix position to a keycode through the current layer stack.
 * When the layers feature is compiled out this always reads layer 0. */
kb_keycode_t kb_keycode_at(keypos_t pos);

/* Inject an event as if a key had been pressed. Used by combos/macros; also
 * useful from the web API later. Runs the full feature chain. */
void kb_send_keycode(kb_keycode_t kc, bool pressed);

/* Emit a keycode straight into the matrix key state, bypassing the feature
 * chain. Features call this once they have decided what a key means. */
void kb_register(kb_keycode_t kc);
void kb_unregister(kb_keycode_t kc);

/* Continue the feature chain from just after `self`, then run the default
 * handler. Used by features that hold events back and replay them later
 * (combos) so a deferred key still passes through tapping, layers, macros. */
void kb_dispatch_after(bool (*self)(keyrecord_t *), keyrecord_t *rec);
