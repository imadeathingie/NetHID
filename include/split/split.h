/*
 * split/split.h — modular keyboards: one primary, any number of modules.
 *
 * A module is a Pico with switches on it. Each has its own pin assignment, its
 * own matrix size, and — in time — its own sensors. They are not "halves" and
 * are not required to resemble each other: a 4x6 left side, a 4x6 right side, a
 * 1x4 macropad and an encoder pod are four modules on the same bus.
 *
 * The primary runs all of NetHID. Every module runs a small firmware that scans
 * its own hardware and answers when polled — no TinyUSB, no lwIP, no cyw43, so
 * a module is happy on a plain Pico.
 *
 * ── Addressing and why the bus is polled ────────────────────────────────────
 * The primary is bus master. It polls module 1, then 2, then 3, and a module
 * transmits ONLY in response to its own address. Collisions are therefore
 * impossible by construction rather than by good luck — which matters because
 * the alternative on a shared wire is modules talking over each other, and a
 * collision does not look like silence, it looks like a corrupted matrix row,
 * which is a fistful of phantom keypresses.
 *
 * The cost is latency: a full cycle is N polls. See SPLIT_UART_BAUD.
 *
 * ── The combined matrix ─────────────────────────────────────────────────────
 * A board lists its modules and each one's shape:
 *
 *     #define SPLIT_MODULES              \
 *         SPLIT_MODULE(0, 4, 6)   // primary, 4 rows x 6 cols
 *         SPLIT_MODULE(1, 4, 6)   // right side
 *         SPLIT_MODULE(2, 1, 4)   // macropad
 *
 * Rows are laid end to end in that order, so module 0 owns rows 0-3, module 1
 * rows 4-7, module 2 row 8. MATRIX_ROWS is the total and MATRIX_COLS the widest.
 * The keymap stays one array, and nothing above the matrix layer knows the
 * keyboard is in pieces.
 *
 * Module 0 is always the primary and is not polled — it scans its own GPIO.
 *
 * ── State, not events ───────────────────────────────────────────────────────
 * A module reports its complete state, never "key 3 went down". A dropped or
 * corrupted frame on a cable running through a jack you can waggle is not rare,
 * and with events a single loss desyncs a module permanently — a key held down
 * until you unplug something. With state, the next poll is a full correction.
 * Same reasoning as the source merge in keystate.cpp.
 *
 * ── Why serial and not wireless ─────────────────────────────────────────────
 * Wireless splits exist, but they are BLE on dedicated radio silicon. Over the
 * CYW43 there is no low-latency peer mode: association and the WiFi/lwIP path
 * cost more than the whole scan budget per key, jitter is tens of milliseconds,
 * and a battery module running WiFi has a short and disappointing life.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

#ifndef SPLIT_ENABLE
#define SPLIT_ENABLE 0
#endif

/* ── Module table ────────────────────────────────────────────────────────────
 * Expanded several ways below. A board defines SPLIT_MODULES; everything else
 * here is derived, so a module's shape is stated exactly once. */
#ifdef SPLIT_MODULES

/* Count, total rows, widest module. */
/* Counts are computed at runtime in split_primary.cpp from the expanded table,
 * not by preprocessor arithmetic. A macro that redefines SPLIT_MODULE to `+1`
 * and undefs it afterwards expands at its USE site, by which point the helper
 * is gone — see the same note in kb/encoder.h. */

/* MATRIX_COLS is stated directly by the board rather than derived: a
 * preprocessor max over a list is unreadable, and the primary checks the total
 * row count against MATRIX_ROWS at startup and complains if they disagree. */

typedef struct {
    uint8_t id;
    uint8_t rows;
    uint8_t cols;
    uint8_t row_offset;      /* where this module's rows start in the matrix */
    uint8_t encoders;        /* how many are wired to it */
    uint8_t encoder_base;    /* its first encoder's index in encoder_map */
} split_module_t;

extern const split_module_t *SPLIT_MODULE_TABLE_PTR;
extern const uint8_t         split_module_count;

/* Rows this firmware scans with its own GPIO. On the primary that is module 0;
 * on a module build it is that module's own row count, which the module header
 * supplies as SPLIT_MODULE_ROWS. */
#ifdef SPLIT_MODULE_ROWS
#define MATRIX_ROWS_LOCAL SPLIT_MODULE_ROWS   /* a module build */
#else
#define MATRIX_ROWS_LOCAL SPLIT_PRIMARY_ROWS  /* the primary: module 0's rows */
#endif

#else   /* not a split board */
#define MATRIX_ROWS_LOCAL MATRIX_ROWS
#endif

/* ── Wire format ─────────────────────────────────────────────────────────────
 *
 *   0xA5  addr  type  len  payload[len]  crc8
 *
 * `addr` is the module the frame concerns: the primary puts the address it is
 * polling, and the module answers with its own. A reply carrying the wrong
 * address is discarded rather than applied to whoever happens to be next in the
 * table — on a shared bus that is the difference between "a module is missing"
 * and "the macropad's keys appear on the left hand".
 *
 * Sync byte because a UART just powered up, or a jack being pushed in, delivers
 * partial frames and the receiver needs somewhere to resynchronise to. CRC8
 * because a marginal cable corrupts bytes rather than losing whole frames.
 */
#define SPLIT_SYNC      0xA5
#define SPLIT_ADDR_HOST 0x00      /* the primary */

typedef enum {
    /* The poll doubles as the status broadcast. Its payload is a packed
     * kb_status_t, so a module gets everything it needs to render a display
     * every cycle without a single extra frame on the bus. A framebuffer is out
     * of the question here — 1 KB against a budget of a few dozen bytes. */
    SPLIT_MSG_POLL    = 0x01,   /* primary -> module: state request + status   */
    SPLIT_MSG_STATE   = 0x02,   /* module -> primary: matrix rows             */
    SPLIT_MSG_HELLO   = 0x03,   /* module -> primary: shape and capabilities  */
    SPLIT_MSG_SENSOR  = 0x04,   /* module -> primary: reserved for encoders,
                                   pointing devices, anything that is not a
                                   key. Typed payload so adding one does not
                                   change the framing.                        */
} split_msg_t;

#define SPLIT_MAX_PAYLOAD 32
#define SPLIT_ROW_BYTES   ((MATRIX_COLS + 7) / 8)

typedef struct {
    uint8_t addr;
    uint8_t type;
    uint8_t len;
    uint8_t payload[SPLIT_MAX_PAYLOAD];
} split_packet_t;

/* CRC8, polynomial 0x07. Shared by both ends so they cannot disagree. */
uint8_t split_crc8(const uint8_t *data, uint8_t len);

/* ── Link ────────────────────────────────────────────────────────────────────
 * `listen_only` puts the transmitter in high-Z when idle. Every module shares
 * one wire back to the primary, so a module that drives it while another is
 * answering corrupts both. */
bool split_link_init(bool listen_only);
bool split_link_send(const split_packet_t *pkt);
bool split_link_recv(split_packet_t *pkt);   /* polls; never waits */

/* ── Primary ────────────────────────────────────────────────────────────────*/
void split_primary_init(void);
void split_primary_task(void);
void split_primary_rows(void *rows);         /* fills every non-local row */
bool split_module_online(uint8_t id);
uint8_t split_modules_online(void);

/* ── Module ─────────────────────────────────────────────────────────────────*/
void split_module_init(void);
void split_module_task(void);
