#pragma once
#include "kb/kb.h"

/* Milliseconds a dual-role key must be held before it counts as a hold. */
#ifndef TAPPING_TERM
#define TAPPING_TERM 200
#endif

/* Resolve as a hold the instant another key is pressed. Right for layer-taps
 * on thumb keys, wrong for home-row mods while typing fast — turn it off and
 * lean on TAPPING_TERM if you get spurious holds mid-word. */
#ifndef HOLD_ON_OTHER_KEY_PRESS
#define HOLD_ON_OTHER_KEY_PRESS 1
#endif

/* Per-keycode override, defined weakly — redefine it in your keymap. */
uint16_t kb_tapping_term(kb_keycode_t kc);
