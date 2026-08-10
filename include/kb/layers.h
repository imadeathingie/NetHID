#pragma once
#include "kb/kb.h"

/* Layer stack. Layer 0 is the default layer unless DF() moves it.
 * Lookup walks from the highest active layer downwards, skipping KC_TRNS. */
void     kb_layer_on(uint8_t layer);
void     kb_layer_off(uint8_t layer);
void     kb_layer_toggle(uint8_t layer);
void     kb_layer_move(uint8_t layer);        /* TO(): clear all, set one */
void     kb_layer_set_default(uint8_t layer);
bool     kb_layer_is_on(uint8_t layer);
uint8_t  kb_layer_highest(void);
uint32_t kb_layer_state(void);                /* for the web UI / debug */

kb_keycode_t kb_layer_lookup(keypos_t pos);
