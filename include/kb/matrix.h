#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

/* One bit per column. Widen if you ever exceed 32 columns on one row. */
typedef uint32_t matrix_row_t;
#if MATRIX_COLS > 32
#error "matrix_row_t is 32 bits — MATRIX_COLS > 32 needs a wider type"
#endif

void matrix_init(void);

/* Read the hardware into `dest` (MATRIX_ROWS entries). Returns true if the
 * raw state differs from the previous scan. */
bool matrix_scan(matrix_row_t *dest);
