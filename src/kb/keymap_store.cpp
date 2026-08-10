/* keymap_store.cpp — see include/kb/keymap_store.h */

#include "kb/kb.h"
#include "kb/keymap_store.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#define KM_MAGIC   0x4D4B484Eu     /* "NHKM" little-endian */
#define KM_VERSION 2

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  rows, cols;
    uint8_t  layers;
    uint8_t  reserved;
    uint32_t crc;      /* over the keycode data */
    uint32_t base_id;  /* fingerprint of the COMPILED keymap this was derived from */
} km_header_t;

#define KM_KEYS      (KB_MAX_LAYERS * MATRIX_ROWS * MATRIX_COLS)
#define KM_ENC_SLOTS (KB_MAX_LAYERS * (NUM_ENCODERS > 0 ? NUM_ENCODERS : 1) * 3)
#define KM_DATA_LEN  ((KM_KEYS + KM_ENC_SLOTS) * (int)sizeof(uint16_t))
#define KM_BLOB_LEN  ((int)sizeof(km_header_t) + KM_DATA_LEN)

/* One page at a time is the granularity flash programming accepts. */
#define KM_PROG_LEN  (((KM_BLOB_LEN + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

static_assert(KM_PROG_LEN <= FLASH_SECTOR_SIZE,
              "keymap blob does not fit one flash sector — raise the sector count or lower KB_MAX_LAYERS");
static_assert(KB_MAX_LAYERS >= 1, "KB_MAX_LAYERS must be at least 1");

/* Last sector of flash. Nothing reserves it in the linker script, so if the
 * firmware ever grows to fill the part this collides — at 4 MB on a Pico 2 W
 * there is no realistic danger, but it is worth knowing. */
#define KM_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/*
 * The live keymap.
 *
 * Core 1 reads this on every scan; core 0 writes it from the HTTP handler.
 * There is deliberately no lock: a uint16_t store to an aligned address is a
 * single instruction on Cortex-M33, so a reader can never observe half a
 * keycode. The worst case is that a keypress landing in the same microsecond
 * as an edit resolves to either the old or the new value, which is exactly
 * what a lock would give you anyway. Taking a spinlock on the scan hot path to
 * buy nothing would be the wrong trade.
 */
static uint16_t live[KB_MAX_LAYERS][MATRIX_ROWS][MATRIX_COLS];

/* Encoders live in their own array but are CRC'd and flashed with the keymap,
 * so one Save persists both. Sized to at least 1 so a board without encoders
 * still compiles without special-casing every loop below. */
#if NUM_ENCODERS > 0
static uint16_t live_enc[KB_MAX_LAYERS][NUM_ENCODERS][3];
#else
static uint16_t live_enc[KB_MAX_LAYERS][1][3];
#endif

static volatile bool save_pending;
static volatile bool dirty;
static bool          have_stored;

/* ── CRC32 (table-less, reflected, standard polynomial) ───────────────────── */
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
 * Fingerprint of the compiled-in keymap.
 *
 * This exists because of a nasty failure mode: edit keymap.cpp, rebuild, flash,
 * and the stored blob from before still matches on rows/cols and still passes
 * CRC — so it silently wins over the keymap you just flashed and half your keys
 * are "wrong" with nothing to explain why. Storing a fingerprint of the compiled
 * defaults means a changed keymap.cpp invalidates the stored copy, which is the
 * safe default: an explicit reflash beats a remembered edit.
 *
 * Set -DKB_KEYMAP_KEEP_ON_REFLASH=1 if you would rather runtime edits survive
 * recompiles, accepting that you then have to reset from the web UI to see a
 * change you made in keymap.cpp.
 */
static uint32_t compiled_id(void) {
    uint32_t c = 0xFFFFFFFFu;
    uint8_t n = keymap_layer_count;
    c = crc32(&n, 1) ^ 0xFFFFFFFFu;
    /* Fold the compiled layers in one keycode at a time — `keymaps` is not
     * necessarily contiguous with KB_MAX_LAYERS, so no bulk memcpy here. */
    for (uint8_t l = 0; l < keymap_layer_count; l++)
        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t co = 0; co < MATRIX_COLS; co++) {
                uint16_t kc = keymaps[l][r][co];
                uint8_t b[2] = { (uint8_t)(kc & 0xFF), (uint8_t)(kc >> 8) };
                c ^= crc32(b, 2);
            }
    return c;
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */
static void load_defaults(void) {
    memset(live, 0, sizeof(live));
    for (uint8_t l = 0; l < keymap_layer_count && l < KB_MAX_LAYERS; l++)
        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t c = 0; c < MATRIX_COLS; c++)
                live[l][r][c] = keymaps[l][r][c];
    /* Layers past the compiled ones start fully transparent, so adding one in
     * the UI does not silently mask the layer below it. */
    for (uint8_t l = keymap_layer_count; l < KB_MAX_LAYERS; l++)
        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t c = 0; c < MATRIX_COLS; c++)
                live[l][r][c] = KC_TRNS;

#if NUM_ENCODERS > 0
    for (uint8_t l = 0; l < KB_MAX_LAYERS; l++)
        for (uint8_t e = 0; e < NUM_ENCODERS; e++)
            for (uint8_t a = 0; a < 3; a++)
                live_enc[l][e][a] = (l < encoder_map_layers)
                                  ? encoder_map[l][e][a] : KC_TRNS;
#endif
}

/* ── Flash ────────────────────────────────────────────────────────────────── */
static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + KM_FLASH_OFFSET);
    km_header_t h;
    memcpy(&h, base, sizeof(h));

    if (h.magic != KM_MAGIC)      return false;
    if (h.version != KM_VERSION)  return false;
    if (h.rows != MATRIX_ROWS || h.cols != MATRIX_COLS) {
        printf("[keymap] stored blob is %ux%u, firmware is %ux%u — ignoring\n",
               h.rows, h.cols, MATRIX_ROWS, MATRIX_COLS);
        return false;
    }
    if (h.layers == 0 || h.layers > KB_MAX_LAYERS) return false;

#if !KB_KEYMAP_KEEP_ON_REFLASH
    if (h.base_id != compiled_id()) {
        printf("[keymap] stored keymap was derived from a different compiled\n"
               "         keymap — discarding it and using the one just flashed.\n"
               "         (build with -DKB_KEYMAP_KEEP_ON_REFLASH=1 to keep it)\n");
        return false;
    }
#endif

    if (crc32(base + sizeof(h), KM_DATA_LEN) != h.crc) {
        printf("[keymap] stored blob failed CRC — ignoring\n");
        return false;
    }

    memcpy(live, base + sizeof(h), sizeof(live));
    memcpy(live_enc, base + sizeof(h) + sizeof(live), sizeof(live_enc));
    printf("[keymap] loaded %u layers (and %d encoder slot(s)) from flash\n",
           h.layers, NUM_ENCODERS);
    return true;
}

/* Must not be inlined into flash-resident code: while the erase is in flight
 * XIP is disabled, so anything executing from flash faults. */
static void __no_inline_not_in_flash_func(flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(KM_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(KM_FLASH_OFFSET, blob, KM_PROG_LEN);
    restore_interrupts(ints);
}

void kb_keymap_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    static uint8_t blob[KM_PROG_LEN];
    memset(blob, 0xFF, sizeof(blob));

    km_header_t h = {};
    h.magic   = KM_MAGIC;
    h.version = KM_VERSION;
    h.rows    = MATRIX_ROWS;
    h.cols    = MATRIX_COLS;
    h.layers  = KB_MAX_LAYERS;
    /* One CRC over keymap and encoders together: they are saved together, so a
     * blob that is half valid is not a state worth being able to represent. */
    static uint8_t crcbuf[KM_DATA_LEN];
    memcpy(crcbuf, live, sizeof(live));
    memcpy(crcbuf + sizeof(live), live_enc, sizeof(live_enc));
    h.crc     = crc32(crcbuf, KM_DATA_LEN);
    h.base_id = compiled_id();

    memcpy(blob, &h, sizeof(h));
    memcpy(blob + sizeof(h), live, sizeof(live));
    memcpy(blob + sizeof(h) + sizeof(live), live_enc, sizeof(live_enc));

    printf("[keymap] committing %d bytes to flash\n", KM_PROG_LEN);

    /*
     * Core 1 is running the USB stack and the matrix scanner out of flash. It
     * must be parked in RAM before XIP goes away, or it faults the moment the
     * erase starts. multicore_lockout does exactly this; core 1 opts in during
     * kb_init() by calling multicore_lockout_victim_init().
     *
     * The cost is real and worth stating plainly: for the ~50-100 ms this
     * takes, USB is not serviced and interrupts are off, so the host sees the
     * device go quiet and a WiFi packet or two may be dropped. TCP recovers,
     * TinyUSB recovers, and it only happens when someone presses Save.
     */
    multicore_lockout_start_blocking();
    flash_commit(blob);
    multicore_lockout_end_blocking();

    have_stored = true;
    dirty = false;
    printf("[keymap] saved\n");
}

/* ── API ──────────────────────────────────────────────────────────────────── */
void kb_keymap_store_init(void) {
    load_defaults();
#if KB_KEYMAP_IGNORE_STORED
    have_stored = false;
    printf("[keymap] KB_KEYMAP_IGNORE_STORED — using the compiled keymap\n");
#else
    have_stored = load_from_flash();
#endif
    /* Always say which keymap is actually in force. Silence here is how you end
     * up debugging a matrix that is fine. */
    printf("[keymap] active source: %s (%ux%u, %u layers in RAM)\n",
           have_stored ? "STORED (flash)" : "COMPILED (keymap.cpp)",
           MATRIX_ROWS, MATRIX_COLS, KB_MAX_LAYERS);
    dirty = false;
    save_pending = false;
}

uint16_t kb_keymap_at(uint8_t layer, uint8_t row, uint8_t col) {
    if (layer >= KB_MAX_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return KC_NO;
    return live[layer][row][col];
}

bool kb_keymap_set(uint8_t layer, uint8_t row, uint8_t col, uint16_t kc) {
    if (layer >= KB_MAX_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return false;
    if (live[layer][row][col] != kc) {
        live[layer][row][col] = kc;
        dirty = true;
    }
    return true;
}

uint8_t kb_keymap_layers(void) { return KB_MAX_LAYERS; }

uint8_t kb_encoder_count(void) { return NUM_ENCODERS; }

uint16_t kb_encoder_at(uint8_t layer, uint8_t index, uint8_t action) {
#if NUM_ENCODERS > 0
    if (layer >= KB_MAX_LAYERS || index >= NUM_ENCODERS || action > 2) return KC_NO;
    return live_enc[layer][index][action];
#else
    (void)layer; (void)index; (void)action;
    return KC_NO;
#endif
}

bool kb_encoder_set(uint8_t layer, uint8_t index, uint8_t action, uint16_t kc) {
#if NUM_ENCODERS > 0
    if (layer >= KB_MAX_LAYERS || index >= NUM_ENCODERS || action > 2) return false;
    if (live_enc[layer][index][action] != kc) {
        live_enc[layer][index][action] = kc;
        dirty = true;
    }
    return true;
#else
    (void)layer; (void)index; (void)action; (void)kc;
    return false;
#endif
}

void kb_keymap_reset(bool erase) {
    load_defaults();
    dirty = true;
    if (erase) {
        /* Erasing means writing the compiled defaults back out, which is
         * simpler than a special "blank" state and leaves flash consistent. */
        kb_keymap_save_request();
    }
}

void kb_keymap_save_request(void) { save_pending = true; }
bool kb_keymap_dirty(void)        { return dirty; }
bool kb_keymap_stored(void)       { return have_stored; }
bool kb_keymap_save_pending(void) { return save_pending; }
