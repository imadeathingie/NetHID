/*
 * matrix.cpp — direct-pin matrix scanning.
 *
 * COL2ROW  diodes point column → row: rows are driven, columns are read.
 * ROW2COL  diodes point row → column: columns are driven, rows are read.
 *
 * Driven lines are held in high-Z and only pulled low while selected, so an
 * accidental short between two driven lines can't fight. Read lines use the
 * RP2350's internal pull-ups (~50k), which is plenty for a keyboard matrix.
 *
 * Without a diode per switch you get ghosting past two simultaneous keys and
 * none of the tap-hold or combo behaviour will be trustworthy. Fit the diodes.
 */

#include "kb/matrix.h"
#if SPLIT_ENABLE
#include "split/split.h"
#endif
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>

#ifndef MATRIX_IO_DELAY_US
#define MATRIX_IO_DELAY_US 10   /* settle time after selecting a line */
#endif

/*
 * On a split, each half wires only its own rows, so these arrays are
 * MATRIX_ROWS_LOCAL long, not MATRIX_ROWS. The two halves use identical pin
 * lists — a row is a row — and differ only in where the result lands in the
 * combined matrix, which split_primary_rows() decides.
 */
#ifndef MATRIX_ROWS_LOCAL
#define MATRIX_ROWS_LOCAL MATRIX_ROWS
#endif

static const uint8_t row_pins[MATRIX_ROWS_LOCAL] = MATRIX_ROW_PINS;
static const uint8_t col_pins[MATRIX_COLS]       = MATRIX_COL_PINS;

static matrix_row_t prev[MATRIX_ROWS_LOCAL];

#if defined(DIODE_DIRECTION) && DIODE_DIRECTION == ROW2COL
#  define DRIVE_PINS col_pins
#  define DRIVE_N    MATRIX_COLS
#  define READ_PINS  row_pins
#  define READ_N     MATRIX_ROWS_LOCAL
#else
#  define DRIVE_PINS row_pins
#  define DRIVE_N    MATRIX_ROWS_LOCAL
#  define READ_PINS  col_pins
#  define READ_N     MATRIX_COLS
#endif

static inline void drive_release(uint8_t pin) {
    gpio_set_dir(pin, GPIO_IN);      /* high-Z */
    gpio_disable_pulls(pin);
}

static inline void drive_assert(uint8_t pin) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);     /* actively low */
}

void matrix_init(void) {
    for (uint8_t i = 0; i < DRIVE_N; i++) {
        gpio_init(DRIVE_PINS[i]);
        gpio_put(DRIVE_PINS[i], 0);
        drive_release(DRIVE_PINS[i]);
    }
    for (uint8_t i = 0; i < READ_N; i++) {
        gpio_init(READ_PINS[i]);
        gpio_set_dir(READ_PINS[i], GPIO_IN);
        gpio_pull_up(READ_PINS[i]);  /* switch closed pulls the line low */
    }
    memset(prev, 0, sizeof(prev));
}

bool matrix_scan(matrix_row_t *dest) {
    /* Only the local rows are cleared and driven here. On a split the rest are
     * owned by split_primary_rows(), which is called after this returns —
     * clearing the whole array would wipe the other half's state every scan. */
    memset(dest, 0, sizeof(matrix_row_t) * MATRIX_ROWS_LOCAL);

    for (uint8_t d = 0; d < DRIVE_N; d++) {
        drive_assert(DRIVE_PINS[d]);
        busy_wait_us_32(MATRIX_IO_DELAY_US);

        uint32_t all = gpio_get_all();
        for (uint8_t r = 0; r < READ_N; r++) {
            if (!(all & (1u << READ_PINS[r]))) {   /* low = pressed */
#if defined(DIODE_DIRECTION) && DIODE_DIRECTION == ROW2COL
                dest[r] |= (matrix_row_t)1u << d;   /* d is a column */
#else
                dest[d] |= (matrix_row_t)1u << r;   /* d is a row     */
#endif
            }
        }

        drive_release(DRIVE_PINS[d]);
        /* Give the line time to float back up before selecting the next one,
         * otherwise a slow rise reads as a phantom press on the next pass. */
        busy_wait_us_32(MATRIX_IO_DELAY_US);
    }

    bool changed = memcmp(dest, prev, sizeof(prev)) != 0;
    if (changed) memcpy(prev, dest, sizeof(prev));
    return changed;
}
