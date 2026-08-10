#pragma once
#include "kb/matrix.h"

/* Symmetric per-key deferred debounce, the same algorithm QMK defaults to
 * (sym_defer_pk): a key's state is copied through only once it has been
 * stable for DEBOUNCE_MS. Cheap, immune to both bounce and chatter, and it
 * costs one full debounce interval of latency on every edge.
 *
 * Swap the implementation by pointing cmake/keyboard.cmake at a different
 * src/kb/debounce_*.cpp — the interface is these two functions. */
void debounce_init(void);

/* `raw` in, `cooked` out. Both are MATRIX_ROWS long. Returns true if
 * `cooked` changed this call. */
bool debounce_run(const matrix_row_t *raw, matrix_row_t *cooked, uint32_t now_ms);
