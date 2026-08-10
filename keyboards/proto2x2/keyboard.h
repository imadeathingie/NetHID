/*
 * proto2x2 — the four-switch breadboard rig.
 *
 * Small enough to wire in five minutes, big enough to exercise layers,
 * mod-taps, combos and one-shots. Build the firmware against this first: it
 * flushes out scan timing and the physical/network report merge while the
 * whole thing is still throwaway.
 *
 *        C0(GP4)  C1(GP5)
 *   R0    ─┬───────┬──   GP2
 *          SW      SW
 *   R1    ─┴───────┴──   GP3
 *
 * One 1N4148 in series with every switch, cathode (banded end) toward the
 * ROW pin. Without them you get ghosting on any third simultaneous key.
 */
#pragma once

#include "kb/diode.h"

#define MATRIX_ROWS 2
#define MATRIX_COLS 2

/* GP2-GP5 are free in the stock config: GP0/1 are UART, GP16-GP19 and GP21/22
 * belong to the IR/RF blaster and receivers. Check include/config.h before
 * reassigning anything. */
#define MATRIX_ROW_PINS { 2, 3 }
#define MATRIX_COL_PINS { 4, 5 }
#define DIODE_DIRECTION COL2ROW

#define DEBOUNCE_MS        5
#define MATRIX_IO_DELAY_US 10

#define LAYOUT( \
    k00, k01,   \
    k10, k11    \
) { { k00, k01 }, \
    { k10, k11 } }

/* Physical layout for the web editor — a 2x2 block is exactly what it looks
 * like. Hundredths of a key unit. */
#define LAYOUT_GEOMETRY \
    KB_KEY(0,0,   0,   0) \
    KB_KEY(0,1, 100,   0) \
    KB_KEY(1,0,   0, 100) \
    KB_KEY(1,1, 100, 100)
