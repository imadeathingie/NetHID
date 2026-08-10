/* autoclick.cpp — see include/kb/autoclick.h */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "kb/autoclick.h"
#include "kb/features.h"
#include "kb/keystate.h"
#if ENABLE_SETTINGS
#include "settings.h"
#endif
#include <string.h>
#include <stdio.h>

#if NUM_AUTOCLICKS > 0

/* The board's list is the DEFAULT. `slots` is a RAM copy the web UI edits, and
 * a stored copy in flash overrides the default at boot. Same three-way
 * arrangement as the dynamic keymap. */
#define AUTOCLICK(id, kc, ms, trig) { kc, ms, trig },
static const autoclick_slot_t DEFAULT_SLOTS[NUM_AUTOCLICKS] = { AUTOCLICKS };
#undef AUTOCLICK

static autoclick_slot_t slots[NUM_AUTOCLICKS];

typedef enum { S_OFF, S_PENDING, S_DOWN, S_UP } phase_t;

static struct {
    phase_t  phase;
    bool     latched;      /* toggled on by taps, as opposed to held */
    uint8_t  taps;
    uint32_t last_tap;
    uint32_t next;         /* when the current phase should end */
    bool     held;
} st[NUM_AUTOCLICKS];

/* Set while autoclick is emitting, so its own output does not come back round
 * and look like someone pressing the trigger key. */
static bool emitting;

static uint16_t interval_for(uint8_t i) {
    uint16_t ms = slots[i].interval_ms;
#if ENABLE_SETTINGS
    /* A non-zero runtime setting overrides every slot. Rate is the thing you
     * want to change by feel, and reflashing to try 80 ms instead of 100 is a
     * miserable loop. */
    uint16_t o = settings()->autoclick_ms;
    if (o) ms = o;
#endif
    if (ms < AC_MIN_INTERVAL_MS) ms = AC_MIN_INTERVAL_MS;
    if (ms > AC_MAX_INTERVAL_MS) ms = AC_MAX_INTERVAL_MS;
    return ms;
}

bool autoclick_active(void) {
    for (int i = 0; i < NUM_AUTOCLICKS; i++)
        if (st[i].phase != S_OFF) return true;
    return false;
}

uint16_t autoclick_interval(void) { return interval_for(0); }

static void stop(uint8_t i) {
    if (st[i].phase == S_DOWN) {
        emitting = true;
        kb_send_keycode(slots[i].target, false);   /* never leave it held */
        emitting = false;
    }
    st[i].phase = S_OFF;
    st[i].latched = false;
}

void autoclick_stop_all(void) {
    for (int i = 0; i < NUM_AUTOCLICKS; i++) stop((uint8_t)i);
}

/* ── Flash store ────────────────────────────────────────────────────────────
 * Fifth sector from the end: keymap (last), macros, wifi, settings, autoclick.
 */
#define AC_MAGIC   0x4341484Eu     /* "NHAC" */
#define AC_VERSION 1
#define AC_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 5 * FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;            /* guards a rebuild that changed NUM_AUTOCLICKS */
    uint32_t crc;
    autoclick_slot_t slots[NUM_AUTOCLICKS];
} ac_blob_t;

#define AC_PROG_LEN (((int)sizeof(ac_blob_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
static_assert(AC_PROG_LEN <= FLASH_SECTOR_SIZE, "autoclick blob does not fit one sector");

static volatile bool save_pending;
static bool          dirty;
static bool          stored;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + AC_FLASH_OFFSET);
    ac_blob_t b;
    memcpy(&b, base, sizeof(b));
    if (b.magic != AC_MAGIC || b.version != AC_VERSION) return false;
    if (b.count != NUM_AUTOCLICKS) {
        printf("[autoclick] stored blob has %u slots, firmware has %d — using defaults\n",
               b.count, NUM_AUTOCLICKS);
        return false;
    }
    if (crc32((const uint8_t *)b.slots, sizeof(b.slots)) != b.crc) {
        printf("[autoclick] stored slots failed CRC — using defaults\n");
        return false;
    }
    memcpy(slots, b.slots, sizeof(slots));
    return true;
}

static void __no_inline_not_in_flash_func(ac_flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(AC_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(AC_FLASH_OFFSET, blob, AC_PROG_LEN);
    restore_interrupts(ints);
}

void autoclick_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    /* Nothing may be mid-repeat across a flash erase: the erase parks core 1,
     * so a slot left in S_DOWN would have its release deferred and the host
     * would see the key held for the whole write. */
    autoclick_stop_all();

    static uint8_t raw[AC_PROG_LEN];
    memset(raw, 0xFF, sizeof(raw));
    ac_blob_t b = {};
    b.magic   = AC_MAGIC;
    b.version = AC_VERSION;
    b.count   = NUM_AUTOCLICKS;
    memcpy(b.slots, slots, sizeof(slots));
    b.crc     = crc32((const uint8_t *)b.slots, sizeof(b.slots));
    memcpy(raw, &b, sizeof(b));

    printf("[autoclick] committing to flash\n");
    multicore_lockout_start_blocking();
    ac_flash_commit(raw);
    multicore_lockout_end_blocking();
    dirty  = false;
    stored = true;
    printf("[autoclick] saved\n");
}

bool autoclick_stored(void)       { return stored; }
bool autoclick_dirty(void)        { return dirty; }
bool autoclick_save_pending(void) { return save_pending; }
void autoclick_save(void)         { save_pending = true; }

void autoclick_reset(bool erase) {
    autoclick_stop_all();
    memcpy(slots, DEFAULT_SLOTS, sizeof(slots));
    dirty = stored;              /* still differs from flash until re-saved */
    if (erase) {
        /* Queue a write of the defaults rather than erasing to 0xFF: a blank
         * sector and a sector holding the defaults behave identically at boot,
         * and this way there is only one code path that touches flash. */
        dirty = true;
        save_pending = true;
    }
}

uint8_t autoclick_count(void) { return NUM_AUTOCLICKS; }

const autoclick_slot_t *autoclick_slot(uint8_t i) {
    return (i < NUM_AUTOCLICKS) ? &slots[i] : NULL;
}

bool autoclick_trigger_valid(uint8_t trigger) {
    if (trigger & ~(AC_HOLD | AC_TAP2 | AC_TAP3)) return false;
    if (!trigger) return false;                     /* could never start */
    /* Both tap counts is not a richer setting, it is an ambiguous one — the
     * process loop resolves it to TAP3 and the TAP2 bit silently does nothing.
     * Reject it so the UI cannot offer a state that does not mean what it says. */
    if ((trigger & AC_TAP2) && (trigger & AC_TAP3)) return false;
    return true;
}

bool autoclick_set_slot(uint8_t i, kb_keycode_t target,
                        uint16_t interval_ms, uint8_t trigger) {
    if (i >= NUM_AUTOCLICKS) return false;
    if (!autoclick_trigger_valid(trigger)) return false;
    if (interval_ms < AC_MIN_INTERVAL_MS || interval_ms > AC_MAX_INTERVAL_MS)
        return false;
    if (target == KC_NO) return false;              /* a slot that clicks nothing */

    /* Stop first, while `slots[i].target` is still the keycode the host has
     * down. Editing a running slot would otherwise release the NEW target and
     * leave the old one held. */
    stop(i);

    slots[i].target      = target;
    slots[i].interval_ms = interval_ms;
    slots[i].trigger     = trigger;
    dirty = true;
    return true;
}

void kb_autoclick_init(void) {
    memset(st, 0, sizeof(st));
    emitting = false;
    memcpy(slots, DEFAULT_SLOTS, sizeof(slots));
    stored = load_from_flash();
    dirty  = false;
    save_pending = false;
    printf("[autoclick] %d slot(s), %s\n", NUM_AUTOCLICKS,
           stored ? "loaded from flash" : "compiled defaults");
}

bool kb_autoclick_process(keyrecord_t *rec) {
    if (emitting) return true;                     /* our own emission */
    if (!kc_in(rec->keycode, QK_AUTOCLICK, QK_AUTOCLICK_MAX)) return true;

    uint8_t i = (uint8_t)(rec->keycode & 0xFF);
    if (i >= NUM_AUTOCLICKS) return false;

    const autoclick_slot_t *s = &slots[i];
    uint32_t now = rec->event.time;
    bool taps_wanted = (s->trigger & (AC_TAP2 | AC_TAP3)) != 0;

    if (rec->event.pressed) {
        st[i].held = true;

        if (taps_wanted) {
            if ((now - st[i].last_tap) > AC_TAP_WINDOW_MS) st[i].taps = 0;
            st[i].taps++;
            st[i].last_tap = now;

            /* Already running? Any press stops it. Turning it off must be at
             * least as easy as turning it on. */
            if (st[i].phase != S_OFF) { stop(i); st[i].taps = 0; return false; }

            uint8_t need = (s->trigger & AC_TAP3) ? 3 : 2;
            if (st[i].taps >= need) {
                st[i].taps = 0;
                st[i].latched = true;
                st[i].phase = S_DOWN;
                st[i].next = now;                   /* fire immediately */
                return false;
            }
        } else if (st[i].phase != S_OFF && !(s->trigger & AC_HOLD)) {
            stop(i);
            return false;
        }
        return false;
    }

    /* Release. */
    st[i].held = false;
    if (st[i].latched) return false;               /* toggled on: keep going */
    if (s->trigger & AC_HOLD) stop(i);
    return false;
}

void kb_autoclick_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < NUM_AUTOCLICKS; i++) {
        const autoclick_slot_t *s = &slots[i];

        /* Start a hold, once the tap window has passed on a key that also
         * counts taps — otherwise a double-tap's second press starts holding
         * and the toggle never gets a chance to fire. */
        if (st[i].phase == S_OFF && st[i].held && (s->trigger & AC_HOLD)) {
            bool taps_wanted = (s->trigger & (AC_TAP2 | AC_TAP3)) != 0;
            if (!taps_wanted || (now - st[i].last_tap) >= AC_TAP_WINDOW_MS) {
                st[i].phase = S_DOWN;
                st[i].next = now;
            }
        }
        if (st[i].phase == S_OFF) continue;
        if ((int32_t)(now - st[i].next) < 0) continue;

        if (st[i].phase == S_DOWN) {
            /* Press, then release on a LATER pass. Both in one scan collapse
             * into a single composed report and the host sees nothing at all —
             * the same trap tapping.cpp and the encoder path work around. */
            emitting = true;
            kb_send_keycode(s->target, true);
            emitting = false;
            st[i].phase = S_UP;
            st[i].next = now + 1;
        } else {
            emitting = true;
            kb_send_keycode(s->target, false);
            emitting = false;
            st[i].phase = S_DOWN;
            st[i].next = now + interval_for((uint8_t)i);
        }
    }
}

#else   /* no autoclick slots on this board */

void kb_autoclick_init(void) {}
bool kb_autoclick_process(keyrecord_t *rec) { (void)rec; return true; }
void kb_autoclick_task(void) {}
bool autoclick_active(void) { return false; }
uint16_t autoclick_interval(void) { return 0; }
void autoclick_stop_all(void) {}

/* The runtime-slot API still has to LINK on a board with no slots — CMake
 * refuses KB_FEATURE_AUTOCLICK without them, but this file is also built with
 * the feature on and NUM_AUTOCLICKS temporarily zero while a board is being
 * brought up. Everything reports "no slots" rather than being absent. */
uint8_t autoclick_count(void) { return 0; }
const autoclick_slot_t *autoclick_slot(uint8_t i) { (void)i; return NULL; }
bool autoclick_trigger_valid(uint8_t t) { (void)t; return false; }
bool autoclick_set_slot(uint8_t i, kb_keycode_t k, uint16_t ms, uint8_t t) {
    (void)i; (void)k; (void)ms; (void)t; return false;
}
bool autoclick_stored(void)       { return false; }
bool autoclick_dirty(void)        { return false; }
bool autoclick_save_pending(void) { return false; }
void autoclick_save(void)         {}
void autoclick_reset(bool erase)  { (void)erase; }
void autoclick_commit_poll(void)  {}

#endif
