/*
 * kb/bootmagic.h — hold a key at power-on to land in the RP2350 bootloader.
 *
 * Two independent ways in:
 *
 *   bootmagic     hold BOOTMAGIC_ROW/COL while plugging the board in. Checked
 *                 on core 0 before USB or WiFi come up, so it works even if
 *                 the firmware is otherwise broken — which is the whole point
 *                 of having it.
 *
 *   QK_BOOT       a keycode you can put on a layer. Requests the reset; core 0
 *                 performs it from its own loop.
 *
 * Why the split: reset_usb_boot() tears the chip down from under whichever
 * core calls it. Doing that from core 1 while core 0 is inside lwIP or the
 * cyw43 SPI transaction is asking for a hang, so the keycode path only ever
 * sets a flag and core 0 does the work.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef KB_FEATURE_BOOTMAGIC
#define KB_FEATURE_BOOTMAGIC 0
#endif

/* Call once from main() on core 0, BEFORE multicore_launch_core1() and before
 * cyw43/USB init. If the magic key is held this does not return. */
void kb_bootmagic(void);

/* Is one matrix position held right now, sampled the same careful way bootmagic
 * samples its own key? Safe to call before core 1 exists; initialises the
 * matrix if nothing else has yet. Used by AP mode for its boot trigger, so the
 * two triggers cannot disagree about what "held" means. */
bool kb_matrix_held_at_boot(uint8_t row, uint8_t col);

/* Request a reset into the bootloader from anywhere (any core, any context). */
void kb_bootloader_request(void);

/* Call from core 0's main loop. Performs a pending request. Does not return
 * if there was one. */
void kb_bootloader_poll(void);
