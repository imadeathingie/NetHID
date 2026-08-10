/*
 * oledpad — a single-board macropad with a status display and a knob.
 *
 * The simplest thing that exercises the display: one Pico 2 W, twelve keys, one
 * encoder, one SSD1306. Not split, so there is no bus, no module headers and no
 * second firmware to flash — build it, flash it, look at the screen.
 *
 *   +-------------------------------+
 *   |   [ 128 x 64 OLED ]     (o)   |   (o) = rotary encoder with push
 *   |                               |
 *   |   1   2   3   4               |
 *   |   5   6   7   8               |
 *   |   9  10  11  12               |
 *   +-------------------------------+
 *
 * ── Wiring ──────────────────────────────────────────────────────────────────
 *   Matrix   rows GP4-GP6, cols GP8-GP11, one 1N4148 per switch, cathode
 *            (banded end) toward the row pin.
 *   OLED     SDA GP2, SCL GP3, VCC 3V3, GND. i2c1. Most panel breakouts have
 *            their own pull-ups; the firmware enables the internal ones too,
 *            which is harmless either way.
 *   Encoder  A GP26, B GP27, push GP28, common pin to GND.
 *
 * Nothing here collides with the IR/RF pins in include/config.h (GP16/17 TX,
 * GP19/21/22 RX) or the console UART on GP0/GP1, so the remote features can
 * stay switched on.
 */
#pragma once

#include "kb/diode.h"

#define MATRIX_ROWS 3
#define MATRIX_COLS 4

#define MATRIX_ROW_PINS { 4, 5, 6 }
#define MATRIX_COL_PINS { 8, 9, 10, 11 }
#define DIODE_DIRECTION COL2ROW

#define DEBOUNCE_MS        5
#define MATRIX_IO_DELAY_US 10

/* ── Display ─────────────────────────────────────────────────────────────────
 * Set OLED_SH1106 to 1 if the picture comes up shifted two pixels with a
 * wrapped stripe down one edge. SH1106 panels are sold as SSD1306 constantly
 * and that is exactly what the difference looks like.
 *
 * OLED_TIMEOUT_MS blanks the panel after ten idle minutes. A macropad showing a
 * static layer name all day is precisely the burn-in pattern to avoid. */
#define OLED_ENABLE     1
#define OLED_I2C_INST   i2c1
#define OLED_SDA_PIN    2
#define OLED_SCL_PIN    3
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_SH1106     0
#define OLED_TITLE      "OLEDPAD"
#define OLED_TIMEOUT_MS 600000

/* ── Encoder ─────────────────────────────────────────────────────────────────
 * Its own pins, not matrix positions. If the knob turns volume the wrong way,
 * set ENCODER_REVERSED rather than swapping the wires — which way is clockwise
 * depends entirely on which of A and B you soldered where. */
#define NUM_LOCAL_ENCODERS 1
#define ENCODERS \
    ENCODER(26, 27, 28)
#define ENCODER_REVERSED 0

/* ── Autoclick ───────────────────────────────────────────────────────────────
 * Three slots, one per trigger style, so all three are reachable on one board:
 *
 *   0  hold to burst left-click at 10/s
 *   1  double-tap to latch a spacebar repeat; any tap stops it
 *   2  both: hold for a fast burst, or triple-tap to leave it running
 *
 * Rate is per slot, but `autoclick_ms` on the SETTINGS tab overrides all of
 * them at runtime — it is the thing you want to change by feel, and reflashing
 * to try 80 ms instead of 100 is a miserable loop. */
#define NUM_AUTOCLICKS 3
#define AUTOCLICKS                                  \
    AUTOCLICK(0, MS_BTN1, 100, AC_HOLD)             \
    AUTOCLICK(1, KC_SPC,   50, AC_TAP2)             \
    AUTOCLICK(2, MS_BTN1,  25, AC_TAP3 | AC_HOLD)

/* Boot keys, all on the top row. Held while plugging in:
 *   (0,0) bootloader   (0,1) quiet boot   (0,2) loud boot   (0,3) WiFi setup */
#define BOOTMAGIC_ROW   0
#define BOOTMAGIC_COL   0
#define QUIET_BOOT_ROW  0
#define QUIET_BOOT_COL  1
#define LOUD_BOOT_ROW   0
#define LOUD_BOOT_COL   2
#define AP_MODE_ROW     0
#define AP_MODE_COL     3

#define LAYOUT( \
    k00, k01, k02, k03, \
    k10, k11, k12, k13, \
    k20, k21, k22, k23  \
) { \
    { k00, k01, k02, k03 }, \
    { k10, k11, k12, k13 }, \
    { k20, k21, k22, k23 }  \
}

/* Physical layout for the web keymap editor: a plain 3x4 block. Hundredths of
 * a key unit. */
#define LAYOUT_GEOMETRY \
    KB_KEY(0,0,   0,   0) \
    KB_KEY(0,1, 100,   0) \
    KB_KEY(0,2, 200,   0) \
    KB_KEY(0,3, 300,   0) \
    KB_KEY(1,0,   0, 100) \
    KB_KEY(1,1, 100, 100) \
    KB_KEY(1,2, 200, 100) \
    KB_KEY(1,3, 300, 100) \
    KB_KEY(2,0,   0, 200) \
    KB_KEY(2,1, 100, 200) \
    KB_KEY(2,2, 200, 200) \
    KB_KEY(2,3, 300, 200)
