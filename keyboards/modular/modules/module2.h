/*
 * Module 2 — a four-key macropad. One row, four columns.
 *
 * Narrower than the others, which is fine: it occupies one row of the combined
 * matrix and leaves columns 4 and 5 unused. Its SPLIT_ROW_BYTES is the board's,
 * not its own, so the frame carries two unused bits per row — a rounding cost
 * of nothing in exchange for every module speaking one wire format.
 */
#pragma once
#define MATRIX_ROW_PINS { 2 }
#define MATRIX_COL_PINS { 3, 4, 5, 6 }
#define SPLIT_MODULE_ROWS 1

/*
 * ...and a rotary encoder, on pins of its own.
 *
 * Not part of the matrix above: an encoder is two quadrature lines plus a push
 * switch, and pretending it is three matrix positions means growing the matrix
 * to fit hardware that is not one.
 *
 *   GP7  A       GP8  B       GP9  push switch
 *
 * Common pin to ground; the firmware enables internal pull-ups on all three.
 */
#define NUM_LOCAL_ENCODERS 1
#define ENCODERS \
    ENCODER(7, 8, 9)

/*
 * This module has a display too — a smaller 128x32 strip.
 *
 * It renders from the status the primary puts in every poll frame, so it shows
 * the same layer and modifiers as the primary without owning a keymap. The
 * 32-pixel height means oled_render_status() drops the WPM and module lines; it
 * checks OLED_HEIGHT rather than assuming 64.
 *
 * i2c0 on GP16/GP17. NOT GP0/GP1, which look free on a module and are not: the
 * module build enables stdio on uart0, so those two carry the console. Sharing
 * them with I2C gives a display that works until the firmware prints something,
 * which is a memorable way to lose an evening.
 *
 * i2c1's usable pins on this module collide with the matrix (GP3-6), the
 * encoder (GP7-9) and the bus (GP20/21), so i2c0 it is.
 */
#define OLED_ENABLE   1
#define OLED_I2C_INST i2c0
#define OLED_SDA_PIN  16
#define OLED_SCL_PIN  17
#define OLED_WIDTH    128
#define OLED_HEIGHT   32
#define OLED_TITLE    "PAD"
