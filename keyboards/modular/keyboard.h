/*
 * modular — a worked example of a multi-module board.
 *
 * Three modules of deliberately different shapes, to show that they need not
 * match:
 *
 *   id 0   4 x 6   the primary: a 3x6 alpha block plus 3 thumb keys  (Pico 2 W)
 *   id 1   4 x 6   the right-hand side, same shape, mirrored         (any Pico)
 *   id 2   1 x 4   a four-key macropad                               (any Pico)
 *
 * Rows are laid end to end in the order listed, so module 0 owns rows 0-3,
 * module 1 rows 4-7 and module 2 row 8. MATRIX_ROWS is the total; MATRIX_COLS
 * is the widest module. A narrow module simply leaves its high columns unused.
 *
 * Adding a fourth module is one line in SPLIT_MODULES, one header in modules/,
 * and a row in the keymap.
 */
#pragma once

#include "kb/diode.h"

/* ── The modules ─────────────────────────────────────────────────────────────
 *            id, rows, cols, encoders */
#define SPLIT_MODULES           \
    SPLIT_MODULE(0, 4, 6, 1)    \
    SPLIT_MODULE(1, 4, 6, 0)    \
    SPLIT_MODULE(2, 1, 4, 1)

#define MATRIX_ROWS 9        /* 4 + 4 + 1; the primary checks this at startup */
#define MATRIX_COLS 6        /* the widest module */

/* Encoders across the whole keyboard: one on the primary, one on the macropad.
 * The keymap is indexed by this total, so a knob can move between modules
 * without rewriting encoder_map. */
#define TOTAL_ENCODERS 2

/* Rows module 0 scans with its own GPIO. */
#define SPLIT_PRIMARY_ROWS 4

/*
 * Per-module pins. Each module has its own header because that is the whole
 * point: different modules have different hardware. The build picks one by
 * -DSPLIT_MODULE=<id>, and the primary build gets module 0.
 */
#if   defined(SPLIT_MODULE_ID) && SPLIT_MODULE_ID == 1
#  include "modules/module1.h"
#elif defined(SPLIT_MODULE_ID) && SPLIT_MODULE_ID == 2
#  include "modules/module2.h"
#else
#  include "modules/module0.h"
#endif

/*
 * Displays are declared PER MODULE, in modules/moduleN.h, not here. Only some
 * of these modules have a panel, and a shared define would build the I2C driver
 * and the font into module 1's firmware for nothing — and, worse, would have it
 * initialise an I2C bus on pins that module does not have wired.
 */

#define DEBOUNCE_MS        5
#define MATRIX_IO_DELAY_US 10

/*
 * Bus. uart1, because uart0 is the console on GP0/GP1. GP20/GP21 are clear of
 * the matrices above; on a board that also fits the IR/RF receivers, check
 * include/config.h first — GP21 is IR_RX_PIN by default.
 *
 * Wiring: primary TX -> every module RX. Every module TX -> primary RX, tied
 * together, with a 10k pull-up. Three conductors no matter how many modules.
 *
 * 115200 is conservative and the poll cycle is per-module, so a long bus is
 * where raising this earns its keep — measure before you do.
 */
#define SPLIT_UART_ID     uart1
#define SPLIT_UART_TX_PIN 20
#define SPLIT_UART_RX_PIN 21
#define SPLIT_UART_BAUD   115200

/* Boot keys are sampled before the bus is up, so they must be on module 0.
 * Keys on any other module cannot work: the firmware that would read them has
 * not started talking yet. */
#define BOOTMAGIC_ROW   0
#define BOOTMAGIC_COL   0
#define QUIET_BOOT_ROW  0
#define QUIET_BOOT_COL  1
#define LOUD_BOOT_ROW   0
#define LOUD_BOOT_COL   2
#define AP_MODE_ROW     0
#define AP_MODE_COL     5

/*
 * Written the way the board looks. The right-hand module's columns are
 * MIRRORED — its leftmost key in the picture is column 5, not column 0.
 * Getting that backwards produces a side that types in reverse, and it is the
 * classic modular-keyboard mistake.
 */
#define LAYOUT( \
    L00, L01, L02, L03, L04, L05,   R05, R04, R03, R02, R01, R00, \
    L10, L11, L12, L13, L14, L15,   R15, R14, R13, R12, R11, R10, \
    L20, L21, L22, L23, L24, L25,   R25, R24, R23, R22, R21, R20, \
              L32, L33, L34,             R34, R33, R32,           \
    P0,  P1,  P2,  P3                                             \
) { \
    { L00, L01, L02, L03, L04, L05 }, \
    { L10, L11, L12, L13, L14, L15 }, \
    { L20, L21, L22, L23, L24, L25 }, \
    { KC_NO, KC_NO, L32, L33, L34, KC_NO }, \
    { R00, R01, R02, R03, R04, R05 }, \
    { R10, R11, R12, R13, R14, R15 }, \
    { R20, R21, R22, R23, R24, R25 }, \
    { KC_NO, KC_NO, R32, R33, R34, KC_NO }, \
    { P0,  P1,  P2,  P3,  KC_NO, KC_NO } \
}
