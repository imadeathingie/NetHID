/*
 * mystery6x6 :: debug — every switch announces where it lives.
 *
 * Press a key, it types its own coordinates: "r2c3 ". Walk the board in
 * physical order, keep the output, and you have the true layout.
 *
 * Two deliberate choices:
 *
 *   1. This keymap does NOT use LAYOUT(). It writes the [row][col] array
 *      directly. LAYOUT() is a claim about how physical positions map to the
 *      matrix, and that claim is the thing under test — routing the debug
 *      keymap through it would let a wrong macro hide the wiring it is
 *      supposed to reveal.
 *
 *   2. All 36 positions are mapped, including the four you believe are
 *      unpopulated. If something types "r5c4" then row 5 has more keys on it
 *      than you thought, which is precisely the kind of thing you cannot
 *      discover from a keymap that omits those slots.
 *
 * Also build with -DKB_DEBUG_MATRIX=1 to get "[matrix] r2 c3 down" on the UART
 * (GP0, 115200). The UART log is the more trustworthy of the two: it happens
 * before the keymap, so it fires even for a position that types nothing, and
 * it will show you ghosting — one press producing three edges means a missing
 * diode.
 *
 *   cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=mystery6x6 -DKEYMAP=debug \
 *            -DKB_DEBUG_MATRIX=1
 */

#include "kb/kb.h"
#include "kb/macros.h"
#include <stdio.h>

#define ID(r, c) KB_MACRO((r) * MATRIX_COLS + (c))

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = {
        { ID(0,0), ID(0,1), ID(0,2), ID(0,3), ID(0,4), ID(0,5) },
        { ID(1,0), ID(1,1), ID(1,2), ID(1,3), ID(1,4), ID(1,5) },
        { ID(2,0), ID(2,1), ID(2,2), ID(2,3), ID(2,4), ID(2,5) },
        { ID(3,0), ID(3,1), ID(3,2), ID(3,3), ID(3,4), ID(3,5) },
        { ID(4,0), ID(4,1), ID(4,2), ID(4,3), ID(4,4), ID(4,5) },
        { ID(5,0), ID(5,1), ID(5,2), ID(5,3), ID(5,4), ID(5,5) },
    },
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);

bool kb_macro_user(uint8_t id, keyrecord_t *rec) {
    if (!rec->event.pressed) return false;

    uint8_t row = (uint8_t)(id / MATRIX_COLS);
    uint8_t col = (uint8_t)(id % MATRIX_COLS);

    char out[12];
    snprintf(out, sizeof(out), "r%uc%u ", row, col);
    kb_macro_string(out);
    printf("[debug] pressed r%u c%u\n", row, col);
    return false;
}
