/*
 * main_module.cpp — entry point for a module.
 *
 * The whole firmware. It scans and it answers; there is deliberately nothing
 * else. No TinyUSB, no lwIP, no cyw43 — which is what lets this run on a plain
 * Pico or Pico 2 rather than needing a W.
 *
 * Build:  cmake .. -DKEYBOARD=<board> -DSPLIT_MODULE=<id>
 */

#include "split/split.h"
#include "pico/stdlib.h"
#include <stdio.h>

int main(void) {
    stdio_init_all();
    sleep_ms(50);
    printf("\n[module %d] NetHID module: %d rows x %d cols\n",
           SPLIT_MODULE_ID, MATRIX_ROWS_LOCAL, MATRIX_COLS);

    split_module_init();

    for (;;) {
        split_module_task();
        /*
         * No sleep. Scanning is the only thing this core does, and the reply is
         * driven by the primary's poll rather than by us, so spinning costs
         * nothing and gives the freshest possible state when the poll arrives.
         */
    }
}
