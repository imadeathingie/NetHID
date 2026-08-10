/* debounce.cpp — symmetric per-key deferred debounce (QMK's sym_defer_pk). */

#include "kb/debounce.h"
#include <string.h>

#ifndef DEBOUNCE_MS
#define DEBOUNCE_MS 5
#endif

/* Per-key countdown, in ms, of how long until a pending change is accepted.
 * 0 means "settled, nothing pending". */
static uint8_t  timers[MATRIX_ROWS][MATRIX_COLS];
static uint32_t last_ms;

void debounce_init(void) {
    memset(timers, 0, sizeof(timers));
    last_ms = 0;
}

bool debounce_run(const matrix_row_t *raw, matrix_row_t *cooked, uint32_t now_ms) {
    uint32_t elapsed = now_ms - last_ms;      /* wraps correctly at 2^32 ms */
    last_ms = now_ms;
    if (elapsed > 255) elapsed = 255;

    bool changed = false;
    bool pending = false;

    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        matrix_row_t diff = raw[r] ^ cooked[r];
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            matrix_row_t bit = (matrix_row_t)1u << c;
            uint8_t *t = &timers[r][c];

            if (diff & bit) {
                if (*t == 0) {
                    *t = DEBOUNCE_MS;          /* start the countdown */
                } else if (*t <= elapsed) {
                    cooked[r] ^= bit;          /* stable long enough — accept */
                    *t = 0;
                    changed = true;
                    continue;
                } else {
                    *t = (uint8_t)(*t - elapsed);
                }
                pending = true;
            } else {
                *t = 0;                        /* bounced back — cancel */
            }
        }
    }

    (void)pending;
    return changed;
}
