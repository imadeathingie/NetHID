#pragma once
#include <stdint.h>
#include <stddef.h>
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE   256u
/* Host build: one sector per store, counted back from the end —
 *   1 keymap  2 macros  3 wifi  4 settings  5 autoclick  6 password
 * plus two spare. This MUST be at least as many as the deepest store uses:
 * autoclick's offset (5 back) was already past the end of a four-sector fake
 * flash, so its host test was reading and writing outside the array and only
 * happened not to crash. Raise this before adding a seventh store. */
#define PICO_FLASH_SIZE_BYTES (8u * FLASH_SECTOR_SIZE)
extern uint8_t fake_flash[PICO_FLASH_SIZE_BYTES];
#define XIP_BASE ((uintptr_t)fake_flash)
void flash_range_erase(uint32_t, size_t);
void flash_range_program(uint32_t, const uint8_t *, size_t);
