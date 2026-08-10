/*
 * split_module.cpp — a module on the bus.
 *
 * Scans its own matrix and answers when its address is polled. Nothing else: no
 * keymap, no layers, no HID. It does not know what any of its switches mean,
 * which is why it needs none of the firmware that does.
 *
 * Raw state is sent, not debounced state. Debounce lives once, on the primary,
 * applied to the whole combined matrix — one implementation and one set of
 * timings for every module. Debouncing here as well would add this module's
 * DEBOUNCE_MS to the primary's, so modules would have measurably different
 * latency from each other and only the primary's would be tunable.
 */

#include "split/split.h"
#include "kb/matrix.h"
#include "kb/encoder.h"
#include "oled/oled.h"
#include "oled/kb_status.h"
#include "pico/stdlib.h"
#include <string.h>

#ifndef SPLIT_MODULE_ID
#error "A module build must define SPLIT_MODULE_ID"
#endif

static matrix_row_t cur[MATRIX_ROWS_LOCAL];

void split_module_init(void) {
    memset(cur, 0, sizeof(cur));
    matrix_init();
    encoder_init();
#if OLED_ENABLE
    oled_init();
#endif
    /* listen_only: this module shares the return line with every other one and
     * must leave it alone except while answering. */
    split_link_init(true);
}

void split_module_task(void) {
    /* Scan continuously rather than only when polled. The poll should be
     * answered with the freshest state available, not with a scan started after
     * the request arrived — that would add a whole scan to every key's latency
     * on top of the poll cycle. */
    matrix_scan(cur);
    encoder_task();

    split_packet_t pkt;
    if (!split_link_recv(&pkt)) return;
    if (pkt.addr != SPLIT_MODULE_ID) return;    /* not for us */
    if (pkt.type != SPLIT_MSG_POLL) return;

#if OLED_ENABLE
    /* The poll carries the primary's status. A module has no keymap and cannot
     * work out what layer 2 means on its own, so this is the only way it can
     * show anything true. */
    if (pkt.len >= KB_STATUS_BYTES) {
        kb_status_t st;
        kb_status_unpack(pkt.payload, &st);
        kb_status_set(&st);
        oled_render_status();
        oled_activity();
    }
#endif

    split_packet_t reply;
    reply.addr = SPLIT_MODULE_ID;
    reply.type = SPLIT_MSG_STATE;
    reply.len  = MATRIX_ROWS_LOCAL * SPLIT_ROW_BYTES;
    for (int r = 0; r < MATRIX_ROWS_LOCAL; r++)
        for (int b = 0; b < SPLIT_ROW_BYTES; b++)
            reply.payload[r * SPLIT_ROW_BYTES + b] = (uint8_t)(cur[r] >> (8 * b));
    split_link_send(&reply);

#if OLED_ENABLE
    /* After replying, never before: the primary is waiting on that frame and a
     * page flush would put I2C time in front of it. */
    oled_task();
#endif

#if NUM_LOCAL_ENCODERS > 0
    /* Encoders go in their own frame rather than being appended to the matrix
     * one. Rotation is a DELTA — consumed once and gone — whereas the matrix is
     * state that can be resent harmlessly. Putting them in one frame would mean
     * either resending deltas (double-counted clicks) or making the matrix
     * consume-once (a dropped frame becomes a stuck key). */
    split_packet_t enc_pkt;
    enc_pkt.addr = SPLIT_MODULE_ID;
    enc_pkt.type = SPLIT_MSG_SENSOR;
    enc_pkt.len  = NUM_LOCAL_ENCODERS + 1;
    int8_t deltas[NUM_LOCAL_ENCODERS];
    uint8_t sw;
    encoder_drain(deltas, &sw);
    bool any = sw != 0;
    for (int i = 0; i < NUM_LOCAL_ENCODERS; i++) {
        enc_pkt.payload[i] = (uint8_t)deltas[i];
        if (deltas[i]) any = true;
    }
    enc_pkt.payload[NUM_LOCAL_ENCODERS] = sw;
    /* Only sent when there is something to say. A delta frame that fails to
     * arrive loses those clicks, which is the right failure: a knob that jumps
     * is worse than a knob that misses a step. */
    if (any && !split_link_send(&enc_pkt))
        encoder_inject(0, deltas, NUM_LOCAL_ENCODERS, sw);   /* put them back */
#endif
}
