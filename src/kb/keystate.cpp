/*
 * keystate.cpp — merged keyboard state across the matrix and the network.
 *
 * Each source owns a 256-bit held-key bitmap plus a real modifier byte and a
 * transient ("weak") modifier byte. Composition ORs the modifiers and walks
 * the union of the bitmaps into the six keycode slots of the boot report.
 *
 * Only 32 bytes per source, and the bitmap is already the shape NKRO wants,
 * so switching to a bitmap report later is a descriptor change plus a
 * different compose function — nothing above this file has to move.
 */

#include "tusb.h"
#include "nethid.h"
#include "kb/keystate.h"
#include "kb/keycodes.h"
#include "pico/sync.h"
#include <string.h>

typedef struct {
    uint8_t bits[32];    /* 256 usages, bit n = usage n held */
    uint8_t mods;        /* from held 0xE0-0xE7 usages */
    uint8_t weak_mods;   /* overlay: LSFT(kc), one-shots, caps word */
} src_state_t;

static src_state_t     srcs[KB_SRC_COUNT];
static hid_keyboard_report_t last_sent;
static volatile bool   dirty;
static spin_lock_t    *lock;

static inline uint32_t lk(void)          { return spin_lock_blocking(lock); }
static inline void     unlk(uint32_t s)  { spin_unlock(lock, s); }

void keystate_init(void) {
    memset(srcs, 0, sizeof(srcs));
    memset(&last_sent, 0, sizeof(last_sent));
    dirty = false;
    lock = spin_lock_instance(spin_lock_claim_unused(true));
}

static void recompute_mods(src_state_t *s) {
    uint8_t m = 0;
    for (uint8_t u = KEY_LEFTCTRL; u <= KEY_RIGHTGUI; u++) {
        if (s->bits[u >> 3] & (1u << (u & 7))) m |= (uint8_t)(1u << (u - KEY_LEFTCTRL));
    }
    s->mods = m;
}

void keystate_press(kb_source_t src, uint8_t usage) {
    if (usage == 0) return;
    uint32_t sv = lk();
    src_state_t *s = &srcs[src];
    s->bits[usage >> 3] |= (uint8_t)(1u << (usage & 7));
    if (usage >= KEY_LEFTCTRL && usage <= KEY_RIGHTGUI) recompute_mods(s);
    dirty = true;
    unlk(sv);
}

void keystate_release(kb_source_t src, uint8_t usage) {
    if (usage == 0) return;
    uint32_t sv = lk();
    src_state_t *s = &srcs[src];
    s->bits[usage >> 3] &= (uint8_t)~(1u << (usage & 7));
    if (usage >= KEY_LEFTCTRL && usage <= KEY_RIGHTGUI) recompute_mods(s);
    dirty = true;
    unlk(sv);
}

void keystate_clear(kb_source_t src) {
    uint32_t sv = lk();
    memset(&srcs[src], 0, sizeof(src_state_t));
    dirty = true;
    unlk(sv);
}

void keystate_set_weak_mods(kb_source_t src, uint8_t mods) {
    uint32_t sv = lk();
    if (srcs[src].weak_mods != mods) { srcs[src].weak_mods = mods; dirty = true; }
    unlk(sv);
}

uint8_t keystate_get_mods(kb_source_t src) {
    return (uint8_t)(srcs[src].mods | srcs[src].weak_mods);
}

void keystate_set_report(kb_source_t src, uint8_t mods, const uint8_t keys[6]) {
    uint32_t sv = lk();
    src_state_t *s = &srcs[src];
    memset(s->bits, 0, sizeof(s->bits));
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u > 1) s->bits[u >> 3] |= (uint8_t)(1u << (u & 7));
    }
    s->mods      = mods;
    s->weak_mods = 0;
    dirty = true;
    unlk(sv);
}

bool keystate_dirty(void) { return dirty; }

void keystate_mark_dirty(void) {
    uint32_t sv = lk();
    /* Forget what we thought we last sent, so the next compose regenerates the
     * report rather than deciding nothing changed. */
    memset(&last_sent, 0xFF, sizeof(last_sent));
    dirty = true;
    unlk(sv);
}

bool keystate_compose(void *out) {
    hid_keyboard_report_t rep = {};
    uint32_t sv = lk();

    for (int i = 0; i < KB_SRC_COUNT; i++)
        rep.modifier |= (uint8_t)(srcs[i].mods | srcs[i].weak_mods);

    uint8_t n = 0;
    bool overflow = false;
    /* Usages 0x04..0xDF: everything that isn't a modifier or reserved. */
    for (uint16_t u = 0x04; u <= 0xDF && !overflow; u++) {
        bool held = false;
        for (int i = 0; i < KB_SRC_COUNT; i++)
            if (srcs[i].bits[u >> 3] & (1u << (u & 7))) { held = true; break; }
        if (!held) continue;
        if (n < 6) rep.keycode[n++] = (uint8_t)u;
        else       overflow = true;
    }
    if (overflow) memset(rep.keycode, KC_ROLL_OVER, sizeof(rep.keycode));

    bool changed = memcmp(&rep, &last_sent, sizeof(rep)) != 0;
    if (changed) last_sent = rep;
    dirty = false;
    unlk(sv);

    if (changed) memcpy(out, &rep, sizeof(rep));
    return changed;
}
