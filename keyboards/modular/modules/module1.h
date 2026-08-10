/*
 * Module 1 — the right-hand side. Same 4x6 shape as module 0 but deliberately
 * DIFFERENT pins, to make the point that modules share nothing but the bus:
 * this one is a different PCB revision with the matrix moved up.
 */
#pragma once
#define MATRIX_ROW_PINS { 2, 3, 4, 5 }
#define MATRIX_COL_PINS { 6, 7, 8, 9, 10, 11 }
#define SPLIT_MODULE_ROWS 4

/* No encoders on this module. */
#define NUM_LOCAL_ENCODERS 0

/* No display on this module. OLED_ENABLE is left undefined, so the driver, the
 * font and the I2C dependency are simply not built into its firmware. */
