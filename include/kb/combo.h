#pragma once
#include "kb/kb.h"

#ifndef COMBO_TERM_MS
#define COMBO_TERM_MS 40      /* all trigger keys must land inside this */
#endif
#ifndef COMBO_MAX_KEYS
#define COMBO_MAX_KEYS 4
#endif

/* A combo is a set of keycodes that, pressed together, produce one action.
 * Trigger lists are KC_NO-terminated. Defined in your keymap:
 *
 *   static const kb_keycode_t combo_esc[] = { KC_J, KC_K, KC_NO };
 *   const kb_combo_t kb_combos[] = { { combo_esc, KC_ESC } };
 *   const uint16_t kb_combo_count = 1;
 */
typedef struct {
    const kb_keycode_t *keys;
    kb_keycode_t        action;
} kb_combo_t;

extern const kb_combo_t kb_combos[];
extern const uint16_t   kb_combo_count;
