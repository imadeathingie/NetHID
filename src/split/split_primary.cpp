/*
 * split_primary.cpp — bus master.
 *
 * Polls each module in turn and keeps its most recent state. The interesting
 * behaviour is what happens when a module stops answering.
 */

#include "split/split.h"
#include "kb/matrix.h"
#include "kb/encoder.h"
#include "oled/kb_status.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

/* The module table, expanded once here from the board's SPLIT_MODULES list. */
#define SPLIT_MODULE(id, rows, cols, encs) { id, rows, cols, 0, encs, 0 },
static split_module_t modules[] = { SPLIT_MODULES };
#undef SPLIT_MODULE

const split_module_t *SPLIT_MODULE_TABLE_PTR = modules;
const uint8_t split_module_count = (uint8_t)(sizeof(modules) / sizeof(modules[0]));

/*
 * How long a module may go unanswered before it is declared gone.
 *
 * Expressed in missed polls rather than milliseconds, because the poll rate
 * depends on how many modules are on the bus: a fixed timeout that is generous
 * with two modules is trigger-happy with six.
 */
#ifndef SPLIT_MISSED_POLLS
#define SPLIT_MISSED_POLLS 8
#endif

/* Gap between a poll going out and giving up on the answer. One frame each way
 * at 115200 is well under a millisecond; this is slack, not a budget. */
#ifndef SPLIT_POLL_TIMEOUT_MS
#define SPLIT_POLL_TIMEOUT_MS 3
#endif

typedef struct {
    matrix_row_t rows[MATRIX_ROWS];   /* only [0, module.rows) are used */
    uint8_t      misses;
    bool         online;
} module_state_t;

static module_state_t state[16];
static uint8_t  polling;          /* index into modules[] */
static uint32_t poll_sent_at;
static bool     awaiting;

static void compute_offsets(void) {
    uint8_t off = 0, enc_off = 0;
    for (uint8_t i = 0; i < split_module_count; i++) {
        modules[i].row_offset    = off;
        modules[i].encoder_base  = enc_off;
        off     = (uint8_t)(off + modules[i].rows);
        enc_off = (uint8_t)(enc_off + modules[i].encoders);
    }
    if (enc_off != NUM_ENCODERS)
        printf("[split] WARNING: modules declare %u encoders but TOTAL_ENCODERS is %d\n",
               enc_off, NUM_ENCODERS);
    if (off != MATRIX_ROWS)
        printf("[split] WARNING: modules total %u rows but MATRIX_ROWS is %d\n",
               off, MATRIX_ROWS);
}

void split_primary_init(void) {
    memset(state, 0, sizeof(state));
    compute_offsets();
    polling = 1;                  /* module 0 is us; never polled */
    awaiting = false;
    split_link_init(false);       /* the primary always drives its own TX */

    printf("[split] primary, %u module(s):\n", split_module_count);
    for (uint8_t i = 0; i < split_module_count; i++)
        printf("[split]   id %u: %ux%u at row %u, %u encoder(s) at %u%s\n",
               modules[i].id, modules[i].rows, modules[i].cols,
               modules[i].row_offset, modules[i].encoders,
               modules[i].encoder_base, i == 0 ? "  (this board)" : "");
}

static void apply_state(uint8_t idx, const split_packet_t *pkt) {
    const split_module_t *m = &modules[idx];
    uint8_t want = (uint8_t)(m->rows * SPLIT_ROW_BYTES);
    /* A module whose reply is the wrong size is running a firmware built for a
     * different shape. Applying it would silently scatter its keys across the
     * matrix, so it is refused and the module stays offline until reflashed. */
    if (pkt->len != want) {
        printf("[split] module %u sent %u bytes, expected %u - wrong firmware?\n",
               m->id, pkt->len, want);
        return;
    }
    for (uint8_t r = 0; r < m->rows; r++) {
        matrix_row_t v = 0;
        for (uint8_t b = 0; b < SPLIT_ROW_BYTES; b++)
            v |= (matrix_row_t)pkt->payload[r * SPLIT_ROW_BYTES + b] << (8 * b);
        state[idx].rows[r] = v;
    }
    state[idx].misses = 0;
    if (!state[idx].online) {
        state[idx].online = true;
        printf("[split] module %u online\n", m->id);
    }
}

static void module_lost(uint8_t idx) {
    state[idx].online = false;
    /*
     * Zero its rows, here, on the transition. Whatever it was holding when the
     * cable came out would otherwise stay held forever: the scan sees no
     * change, so no release event is generated, and the host is left with a key
     * down that nothing on this board can lift.
     */
    memset(state[idx].rows, 0, sizeof(state[idx].rows));
    printf("[split] module %u lost\n", modules[idx].id);
}

void split_primary_task(void) {
    if (split_module_count < 2) return;      /* nothing on the bus */
    uint32_t now = to_ms_since_boot(get_absolute_time());

    /* Collect whatever has arrived. A reply is only accepted from the module we
     * actually asked: on a shared line, applying someone else's frame to the
     * current slot is how a macropad's keys end up on the left hand. */
    split_packet_t pkt;
    while (split_link_recv(&pkt)) {
        if (!awaiting) continue;
        if (pkt.addr != modules[polling].id) continue;
        if (pkt.type == SPLIT_MSG_STATE) {
            apply_state(polling, &pkt);
            awaiting = false;
        }
#if NUM_ENCODERS > 0
        else if (pkt.type == SPLIT_MSG_SENSOR && pkt.len >= 1) {
            /* Encoder deltas from a module, folded in as if they were local.
             * The base index comes from the module table, so a knob's position
             * in encoder_map does not change when it moves between modules. */
            uint8_t n = (uint8_t)(pkt.len - 1);
            encoder_inject(modules[polling].encoder_base, (const int8_t *)pkt.payload,
                           n, pkt.payload[n]);
        }
#endif
    }

    if (awaiting && (now - poll_sent_at) < SPLIT_POLL_TIMEOUT_MS) return;

    if (awaiting) {                          /* no answer in time */
        if (state[polling].online && ++state[polling].misses >= SPLIT_MISSED_POLLS)
            module_lost(polling);
        else if (!state[polling].online)
            state[polling].misses = SPLIT_MISSED_POLLS;
    }

    /* Next module, wrapping past 0 which is us. */
    polling = (uint8_t)(polling + 1);
    if (polling >= split_module_count) polling = 1;

    split_packet_t poll = {};
    poll.addr = modules[polling].id;
    poll.type = SPLIT_MSG_POLL;
    /* Ride the status along with the poll rather than sending it separately.
     * Six bytes on a frame that was going out anyway, and every module is
     * refreshed every cycle. */
    kb_status_t st;
    kb_status_get(&st);
    st.modules = 1;                                   /* ourselves */
    for (uint8_t i = 1; i < split_module_count; i++)
        if (state[i].online) st.modules |= (uint16_t)(1u << modules[i].id);
    kb_status_pack(&st, poll.payload);
    poll.len = KB_STATUS_BYTES;
    split_link_send(&poll);
    poll_sent_at = now;
    awaiting = true;
}

void split_primary_rows(void *rows) {
    matrix_row_t *dest = (matrix_row_t *)rows;
    for (uint8_t i = 1; i < split_module_count; i++) {
        const split_module_t *m = &modules[i];
        for (uint8_t r = 0; r < m->rows; r++)
            dest[m->row_offset + r] = state[i].rows[r];
    }
}

bool split_module_online(uint8_t id) {
    for (uint8_t i = 0; i < split_module_count; i++)
        if (modules[i].id == id) return i == 0 ? true : state[i].online;
    return false;
}

uint8_t split_modules_online(void) {
    uint8_t n = 1;                                  /* ourselves */
    for (uint8_t i = 1; i < split_module_count; i++) if (state[i].online) n++;
    return n;
}
