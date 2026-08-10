/* kb_status.cpp — see include/oled/kb_status.h */

#include "oled/kb_status.h"
#include <string.h>

/*
 * Written by core 1, read by core 0. No lock: every field is a byte or an
 * aligned uint16_t, so no reader can observe a torn value, and the worst case
 * is a frame rendered from a snapshot that is one scan old. A spinlock on the
 * scan hot path to prevent a display being 1 ms stale would be the wrong trade
 * — the same reasoning as the keymap store.
 */
static volatile kb_status_t live;

void kb_status_set(const kb_status_t *s) {
    live.layer   = s->layer;
    live.mods    = s->mods;
    live.flags   = s->flags;
    live.wpm     = s->wpm;
    live.modules = s->modules;
}

void kb_status_get(kb_status_t *out) {
    out->layer   = live.layer;
    out->mods    = live.mods;
    out->flags   = live.flags;
    out->wpm     = live.wpm;
    out->modules = live.modules;
}

void kb_status_pack(const kb_status_t *s, uint8_t out[KB_STATUS_BYTES]) {
    out[0] = s->layer;
    out[1] = s->mods;
    out[2] = s->flags;
    out[3] = s->wpm;
    out[4] = (uint8_t)(s->modules & 0xFF);
    out[5] = (uint8_t)(s->modules >> 8);
}

void kb_status_unpack(const uint8_t in[KB_STATUS_BYTES], kb_status_t *out) {
    out->layer   = in[0];
    out->mods    = in[1];
    out->flags   = in[2];
    out->wpm     = in[3];
    out->modules = (uint16_t)(in[4] | (in[5] << 8));
}
