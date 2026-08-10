/*
 * mystery6x6 — a 6x6 matrix with 32 populated positions.
 *
 * Rename this directory once you have confirmed the wiring; nothing outside it
 * refers to the name except the -DKEYBOARD= argument.
 *
 *   k00 k01 k02 k03 k04 k05
 *   k10 k11 k12 k13 k14 k15
 *   k20 k21 k22 k23 k24 k25
 *   k30 k31 k32 k33 k34 k35
 *             k42 k43
 *                       k44 k45
 *                               k40
 *                          k41 k50
 *                              k51
 *
 * 4x6 main block plus an eight-key cluster. Rows 4 and 5 are the uncertain
 * part, which is exactly what keymaps/debug/ is for.
 *
 * Pins: rows on GP4-GP9, columns on GP10-GP15. Clear of everything else the
 * firmware claims — GP0/1 are the UART console, GP16/17 the IR and 433 MHz
 * transmitters, GP19/21/22 the receivers, GP23-25/29 the CYW43.
 */
#pragma once

#include "kb/diode.h"

#define MATRIX_ROWS 6
#define MATRIX_COLS 6

#define MATRIX_ROW_PINS { 4, 5, 6, 7, 8, 9 }
#define MATRIX_COL_PINS { 10, 11, 12, 13, 14, 15 }
#define DIODE_DIRECTION COL2ROW

#define DEBOUNCE_MS        5
#define MATRIX_IO_DELAY_US 10

/* Boot-key positions (bootmagic, AP mode, quiet boot) are set in
 * include/config.h. This board takes the defaults: top-left, top-right, and
 * second-from-left on the top row. Define them here to override for this board
 * only — config.h guards each with #ifndef. */

/*
 * Autoclick slots, selected from a keymap with AUTOCLK(n). The count is STATED
 * — see the note in kb/autoclick.h for why deriving it from the list does not
 * work.
 *
 *   0  hold to burst left-click at 10/s
 *   1  double-tap to latch a spacebar repeat; any tap stops it
 *   2  both: hold for a fast burst, or triple-tap to leave it running
 *
 * Rate is per slot, but `autoclick_ms` on the SETTINGS tab overrides all of
 * them at runtime, which is the sane way to find a rate you like.
 */
#define NUM_AUTOCLICKS 3
#define AUTOCLICKS                                  \
    AUTOCLICK(0, MS_BTN1, 100, AC_HOLD)             \
    AUTOCLICK(1, KC_SPC,   50, AC_TAP2)             \
    AUTOCLICK(2, MS_BTN1,  25, AC_TAP3 | AC_HOLD)

/*
 * Visual order in, [row][col] out. The four unpopulated positions — (5,2)
 * through (5,5) — are filled with KC_NO here so keymaps never have to mention
 * them.
 *
 * If the debug keymap tells you a key is somewhere other than where this macro
 * puts it, THIS is the thing to fix, not your keymaps.
 */
#define LAYOUT( \
    k00, k01, k02, k03, k04, k05, \
    k10, k11, k12, k13, k14, k15, \
    k20, k21, k22, k23, k24, k25, \
    k30, k31, k32, k33, k34, k35, \
                   k42, k43,      \
                        k44, k45, \
                             k40, \
                        k41, k50, \
                             k51  \
) { \
    { k00, k01, k02, k03, k04, k05 }, \
    { k10, k11, k12, k13, k14, k15 }, \
    { k20, k21, k22, k23, k24, k25 }, \
    { k30, k31, k32, k33, k34, k35 }, \
    { k40, k41, k42, k43, k44, k45 }, \
    { k50, k51, KC_NO, KC_NO, KC_NO, KC_NO } \
}

/*
 * Physical layout, used by the web keymap editor to draw your board instead of
 * a rows×cols grid. Hundredths of a key unit, origin top-left.
 *
 * HEALTH WARNING: the 4x6 main block below is certain, but the eight cluster
 * keys are a direct transcription of the ASCII sketch of your layout — the
 * cascade is what the indentation implied, not measured geometry. Fix it the
 * easy way rather than by editing numbers:
 *
 *   python3 tools/keyboard/kle_to_layout.py --to-kle keyboards/mystery6x6/keyboard.h
 *   # paste into keyboard-layout-editor.com's Raw data box, drag keys around,
 *   # copy the Raw data back out to layout.json, then:
 *   python3 tools/keyboard/kle_to_layout.py layout.json
 *
 * and replace this block with what it prints. The "row,col" top-left legends
 * survive the round trip, so the matrix mapping cannot get lost in the middle.
 */
#define LAYOUT_GEOMETRY \
    KB_KEY(0,0,    0,   0) \
    KB_KEY(0,1,  100,   0) \
    KB_KEY(0,2,  200,   0) \
    KB_KEY(0,3,  300,   0) \
    KB_KEY(0,4,  400,   0) \
    KB_KEY(0,5,  500,   0) \
    KB_KEY(1,0,    0, 100) \
    KB_KEY(1,1,  100, 100) \
    KB_KEY(1,2,  200, 100) \
    KB_KEY(1,3,  300, 100) \
    KB_KEY(1,4,  400, 100) \
    KB_KEY(1,5,  500, 100) \
    KB_KEY(2,0,    0, 200) \
    KB_KEY(2,1,  100, 200) \
    KB_KEY(2,2,  200, 200) \
    KB_KEY(2,3,  300, 200) \
    KB_KEY(2,4,  400, 200) \
    KB_KEY(2,5,  500, 200) \
    KB_KEY(3,0,    0, 300) \
    KB_KEY(3,1,  100, 300) \
    KB_KEY(3,2,  200, 300) \
    KB_KEY(3,3,  300, 300) \
    KB_KEY(3,4,  400, 300) \
    KB_KEY(3,5,  500, 300) \
    KB_KEY(4,2,  200, 400) \
    KB_KEY(4,3,  300, 400) \
    KB_KEY(4,4,  400, 500) \
    KB_KEY(4,5,  500, 500) \
    KB_KEY(4,0,  550, 600) \
    KB_KEY(4,1,  500, 700) \
    KB_KEY(5,0,  600, 700) \
    KB_KEY(5,1,  550, 800)
