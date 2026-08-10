/* bootmagic.cpp — see include/kb/bootmagic.h */

#include "kb/bootmagic.h"
#include "kb/matrix.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/version.h"
#include <stdio.h>

/* Key positions and the sampling window are configured in include/config.h,
 * alongside the AP-mode and quiet-boot keys, so all three boot gestures are
 * described in one place. A board may override any of them in its keyboard.h. */
#include "config.h"

static volatile bool boot_requested;

static inline bool held(const matrix_row_t *m, uint8_t r, uint8_t c) {
    return (m[r] & ((matrix_row_t)1u << c)) != 0;
}

static void enter_bootloader(void) {
    printf("[bootmagic] entering USB bootloader (BOOTSEL)\n");
    /* Flash the onboard LED here if you like — but it hangs off the CYW43,
     * which is not up yet at bootmagic time, so there is nothing to flash. */
    sleep_ms(20);                 /* let the UART drain */

    /* SDK 2.0 renamed this and deprecated the old spelling; keep both so the
     * file builds against either. Needs pico_bootrom linked — cmake/keyboard.cmake
     * does that when KB_FEATURE_BOOTMAGIC is on. */
#if defined(PICO_SDK_VERSION_MAJOR) && PICO_SDK_VERSION_MAJOR >= 2
    rom_reset_usb_boot(0, 0);
#else
    reset_usb_boot(0, 0);
#endif
    for (;;) tight_loop_contents();   /* not reached */
}

/* Shared by kb_bootmagic() and the AP-mode trigger. The window matters: the
 * switch is mechanical and the 3.3 V rail has only just come up, so one scan
 * is not evidence. The key must read down in every scan across ~32 ms. */
static bool held_across_window(uint8_t row, uint8_t col) {
    matrix_row_t m[MATRIX_ROWS];
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) return false;
    for (int i = 0; i < BOOTMAGIC_SCANS; i++) {
        matrix_scan(m);
        if (!held(m, row, col)) return false;
        sleep_ms(BOOTMAGIC_SCAN_INTERVAL_MS);
    }
    return true;
}

static bool matrix_ready;

bool kb_matrix_held_at_boot(uint8_t row, uint8_t col) {
    if (!matrix_ready) { matrix_init(); matrix_ready = true; }
    return held_across_window(row, col);
}

void kb_bootmagic(void) {
    matrix_row_t m[MATRIX_ROWS];

    if (!matrix_ready) { matrix_init(); matrix_ready = true; }

    (void)m;
    if (!held_across_window(BOOTMAGIC_ROW, BOOTMAGIC_COL)) return;
#if defined(BOOTMAGIC_ROW_2) && defined(BOOTMAGIC_COL_2)
    if (!held_across_window(BOOTMAGIC_ROW_2, BOOTMAGIC_COL_2)) return;
#endif

    enter_bootloader();
}

void kb_bootloader_request(void) { boot_requested = true; }

void kb_bootloader_poll(void) {
    if (boot_requested) enter_bootloader();
}
