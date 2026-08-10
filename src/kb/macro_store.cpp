/* macro_store.cpp — see include/kb/macro_store.h */

#include "kb/kb.h"
#include "kb/macro_store.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#define MC_MAGIC   0x434D484Eu     /* "NHMC" */
#define MC_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  count;
    uint8_t  reserved;
    uint32_t crc;
    uint16_t pool_len;
    uint16_t reserved2;
    uint16_t offsets[KB_MACRO_COUNT];
} mc_header_t;

#define MC_BLOB_LEN  ((int)sizeof(mc_header_t) + KB_MACRO_POOL)
#define MC_PROG_LEN  (((MC_BLOB_LEN + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

static_assert(MC_PROG_LEN <= FLASH_SECTOR_SIZE,
              "macro blob does not fit one flash sector — lower KB_MACRO_POOL or KB_MACRO_COUNT");

/* One sector below the keymap's, which owns the last. */
#define MC_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)

#define NO_MACRO 0xFFFF

static uint16_t offsets[KB_MACRO_COUNT];
static uint8_t  pool[KB_MACRO_POOL];
static uint16_t pool_len;

static volatile bool save_pending;
static volatile bool dirty;
static bool          have_stored;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

/*
 * Verify before storing, never while executing.
 *
 * The interpreter runs on core 1 in the scan loop with no bounds checking in
 * its hot path; the guarantee that it cannot run off the end of the pool comes
 * from here. A body must be well-formed, terminated, and self-contained.
 */
static bool verify(const uint8_t *b, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {
        switch (b[i]) {
        case MOP_END:
            return i == len - 1;              /* END must be the last byte */
        case MOP_TAP: case MOP_DOWN: case MOP_UP:
            if (i + 2 >= len) return false;
            i += 3;
            break;
        case MOP_DELAY:
            if (i + 2 >= len) return false;
            i += 3;
            break;
        case MOP_TEXT: {
            if (i + 1 >= len) return false;
            uint8_t n = b[i + 1];
            if (n == 0 || n > KB_MACRO_TEXT_MAX) return false;
            if (i + 2 + n > len) return false;
            for (uint8_t k = 0; k < n; k++) {
                uint8_t ch = b[i + 2 + k];
                if (ch < 0x09 || ch > 0x7E) return false;   /* printable + tab/LF */
            }
            i += 2 + n;
            break;
        }
        default:
            return false;                     /* unknown opcode */
        }
    }
    return false;                             /* fell off the end without END */
}

/*
 * Length of the program at `b`, including its END byte. 0 if malformed.
 *
 * This has to WALK the opcodes. Scanning for the first MOP_END byte is wrong
 * and quietly so: MOP_END is 0x00, and 0x00 is a perfectly ordinary operand —
 * "no key", "no modifiers", the low byte of a delay under 256 ms. A byte scan
 * truncates {DOWN, shift, 0x00, ...} to three bytes and the rest of the macro
 * silently vanishes.
 */
static uint16_t body_len(const uint8_t *b, uint16_t max) {
    uint16_t i = 0;
    while (i < max) {
        switch (b[i]) {
        case MOP_END:  return (uint16_t)(i + 1);
        case MOP_TAP: case MOP_DOWN: case MOP_UP: case MOP_DELAY:
            if (i + 2 >= max) return 0;
            i += 3;
            break;
        case MOP_TEXT:
            if (i + 1 >= max) return 0;
            if (i + 2 + b[i + 1] > max) return 0;
            i += 2 + b[i + 1];
            break;
        default:
            return 0;
        }
    }
    return 0;
}

const uint8_t *kb_macro_body(uint8_t id, uint16_t *len_out) {
    if (id >= KB_MACRO_COUNT) return NULL;
    uint16_t off = offsets[id];
    if (off == NO_MACRO || off >= pool_len) return NULL;
    uint16_t n = body_len(&pool[off], (uint16_t)(pool_len - off));
    if (!n) return NULL;
    if (len_out) *len_out = n;
    return &pool[off];
}

/* Rebuild the pool from scratch with one entry replaced. Compaction on every
 * edit rather than a free list: macros are small, edits are rare and
 * human-initiated, and a pool that cannot fragment is a pool that cannot
 * slowly stop accepting a macro that clearly ought to fit. */
bool kb_macro_set(uint8_t id, const uint8_t *body, uint16_t len) {
    if (id >= KB_MACRO_COUNT) return false;
    if (len && !verify(body, len)) return false;

    static uint8_t  np[KB_MACRO_POOL];
    static uint16_t no[KB_MACRO_COUNT];
    uint16_t n = 0;

    for (uint8_t i = 0; i < KB_MACRO_COUNT; i++) {
        const uint8_t *src;
        uint16_t slen = 0;
        if (i == id) { src = len ? body : NULL; slen = len; }
        else         { src = kb_macro_body(i, &slen); }

        if (!src || !slen) { no[i] = NO_MACRO; continue; }
        if (n + slen > KB_MACRO_POOL) return false;      /* out of room, nothing changed */
        no[i] = n;
        memcpy(np + n, src, slen);
        n = (uint16_t)(n + slen);
    }

    memcpy(pool, np, n);
    memcpy(offsets, no, sizeof(offsets));
    pool_len = n;
    dirty = true;
    return true;
}

void kb_macro_clear_all(void) {
    for (uint8_t i = 0; i < KB_MACRO_COUNT; i++) offsets[i] = NO_MACRO;
    pool_len = 0;
    dirty = true;
}

uint16_t kb_macro_pool_used(void) { return pool_len; }
uint16_t kb_macro_pool_size(void) { return KB_MACRO_POOL; }

static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + MC_FLASH_OFFSET);
    mc_header_t h;
    memcpy(&h, base, sizeof(h));

    if (h.magic != MC_MAGIC || h.version != MC_VERSION) return false;
    if (h.count != KB_MACRO_COUNT) return false;
    if (h.pool_len > KB_MACRO_POOL) return false;
    if (crc32(base + sizeof(h), h.pool_len) != h.crc) {
        printf("[macro] stored macros failed CRC — ignoring\n");
        return false;
    }

    memcpy(offsets, h.offsets, sizeof(offsets));
    memcpy(pool, base + sizeof(h), h.pool_len);
    pool_len = h.pool_len;

    /* Re-verify every body on load. Flash is not adversarial, but a truncated
     * write or a half-erased sector that still CRCs would otherwise hand the
     * interpreter a malformed program. */
    for (uint8_t i = 0; i < KB_MACRO_COUNT; i++) {
        uint16_t n = 0;
        const uint8_t *b = kb_macro_body(i, &n);
        if (b && !verify(b, n)) {
            printf("[macro] macro %u failed verification — dropping it\n", i);
            offsets[i] = NO_MACRO;
        }
    }

    int live = 0;
    for (uint8_t i = 0; i < KB_MACRO_COUNT; i++) if (offsets[i] != NO_MACRO) live++;
    printf("[macro] loaded %d macro(s), %u/%d pool bytes\n", live, pool_len, KB_MACRO_POOL);
    return true;
}

static void __no_inline_not_in_flash_func(mc_flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(MC_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(MC_FLASH_OFFSET, blob, MC_PROG_LEN);
    restore_interrupts(ints);
}

void kb_macro_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    static uint8_t blob[MC_PROG_LEN];
    memset(blob, 0xFF, sizeof(blob));

    mc_header_t h = {};
    h.magic    = MC_MAGIC;
    h.version  = MC_VERSION;
    h.count    = KB_MACRO_COUNT;
    h.pool_len = pool_len;
    h.crc      = crc32(pool, pool_len);
    memcpy(h.offsets, offsets, sizeof(offsets));

    memcpy(blob, &h, sizeof(h));
    memcpy(blob + sizeof(h), pool, pool_len);

    printf("[macro] committing to flash\n");
    multicore_lockout_start_blocking();
    mc_flash_commit(blob);
    multicore_lockout_end_blocking();

    have_stored = true;
    dirty = false;
    printf("[macro] saved\n");
}

void kb_macro_store_init(void) {
    kb_macro_clear_all();
    have_stored = load_from_flash();
    dirty = false;
    save_pending = false;
}

void kb_macro_save_request(void) { save_pending = true; }
bool kb_macro_dirty(void)        { return dirty; }
bool kb_macro_stored(void)       { return have_stored; }
