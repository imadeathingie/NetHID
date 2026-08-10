/*
 * macros.cpp — KB_MACRO(n): stored bytecode first, compiled C second.
 *
 * A macro id resolves in this order:
 *   1. a stored macro from the web UI, if one exists for that id
 *   2. kb_macro_user(), the weak hook a keymap.cpp can override
 *   3. nothing
 *
 * Stored macros win so that editing one in the browser takes effect without a
 * reflash, which is the entire point of them. A keymap that wants an id
 * permanently owned by C should simply not create a stored macro for it.
 *
 * ── The interpreter ─────────────────────────────────────────────────────────
 * Runs on core 1 inside the scan loop, so it must NEVER block: a sleep here
 * stalls USB and the matrix together. It is a state machine advanced once per
 * kb_macros_task(), which is once per scan.
 *
 * Two waits it has to respect and one it does not:
 *
 *   WAIT_FLUSH  a press and its release inside one scan collapse into a single
 *               composed report and the host never sees the key at all. So a
 *               tap presses, waits for hid_task() to compose and send, then
 *               releases. Same problem tapping.cpp solves the same way.
 *   WAIT_TEXT   the typer drives the endpoint directly and the composer stands
 *               aside while it runs, so the macro must too.
 *   DELAY       just a timestamp.
 *
 * One macro runs at a time. A second macro key pressed mid-run is ignored
 * rather than queued or preempting: queueing invites a pile-up from a key that
 * bounces, and preempting leaves the first macro's held keys stranded.
 */

#include "pico/stdlib.h"
#include "tusb.h"
#include "nethid.h"
#include "kb/macros.h"
#include "kb/features.h"
#if KB_FEATURE_MACRO_STORE
#include "kb/macro_store.h"
#include "kb/keystate.h"
#endif
#include <string.h>

__attribute__((weak)) bool kb_macro_user(uint8_t id, keyrecord_t *rec) {
    (void)id; (void)rec;
    return false;
}

void kb_macro_string(const char *s) {
    size_t len = strlen(s);
    if (len > 255) len = 255;
    hid_push_type_string(s, (uint8_t)len, 0);
}

#if KB_FEATURE_MACRO_STORE

/* Longest a single step may wait on the host before we give up and move on.
 * Covers a suspended or unplugged host: without it a macro would wedge. */
#define MACRO_STEP_TIMEOUT_MS 250

typedef enum { MS_IDLE, MS_RUN, MS_DELAY, MS_FLUSH, MS_TEXT } mstate_t;

static struct {
    mstate_t       state;
    const uint8_t *pc;
    const uint8_t *end;
    uint32_t       wake;        /* MS_DELAY: when to resume */
    uint32_t       started;     /* MS_FLUSH / MS_TEXT: for the timeout */

    /* Release payload for the tap currently in MS_FLUSH. */
    uint8_t        tap_mods, tap_key;

    /* Everything DOWN has pressed and not yet released, so an unbalanced
     * program cannot leave a key stuck down forever. */
    uint8_t        held[8];
    uint8_t        held_n;
    uint8_t        held_mods;

    /* Modifiers already down when the macro started. A macro must not release
     * a modifier the user is physically holding — keystate derives its mod byte
     * from held usages, so a blind release would clear the user's shift. */
    uint8_t        entry_mods;
} run;

static void mods_press(uint8_t mods) {
    for (uint8_t i = 0; i < 8; i++)
        if (mods & (1u << i)) keystate_press(KB_SRC_MATRIX, (uint8_t)(KEY_LEFTCTRL + i));
}

static void mods_release(uint8_t mods) {
    mods = (uint8_t)(mods & ~run.entry_mods);      /* never steal the user's */
    for (uint8_t i = 0; i < 8; i++)
        if (mods & (1u << i)) keystate_release(KB_SRC_MATRIX, (uint8_t)(KEY_LEFTCTRL + i));
}

static void note_held(uint8_t key) {
    if (!key || run.held_n >= sizeof(run.held)) return;
    for (uint8_t i = 0; i < run.held_n; i++) if (run.held[i] == key) return;
    run.held[run.held_n++] = key;
}

static void drop_held(uint8_t key) {
    for (uint8_t i = 0; i < run.held_n; i++)
        if (run.held[i] == key) {
            run.held[i] = run.held[--run.held_n];
            return;
        }
}

static void finish(void) {
    for (uint8_t i = 0; i < run.held_n; i++) keystate_release(KB_SRC_MATRIX, run.held[i]);
    mods_release(run.held_mods);
    run.held_n = 0;
    run.held_mods = 0;
    run.state = MS_IDLE;
}

static bool macro_start(uint8_t id) {
    uint16_t len = 0;
    const uint8_t *b = kb_macro_body(id, &len);
    if (!b || !len) return false;
    if (run.state != MS_IDLE) return true;      /* already running: claim, ignore */

    memset(&run, 0, sizeof(run));
    run.pc         = b;
    run.end        = b + len;
    run.state      = MS_RUN;
    run.entry_mods = keystate_get_mods(KB_SRC_MATRIX);
    return true;
}

/* Execute steps until one of them has to wait. Bounds are guaranteed by
 * kb_macro_set()/load_from_flash() having verified the body, so the hot path
 * here does no re-checking beyond the END sentinel. */
static void macro_step(uint32_t now) {
    while (run.pc < run.end) {
        uint8_t op = *run.pc;

        if (op == MOP_END) { finish(); return; }

        if (op == MOP_TAP) {
            uint8_t mods = run.pc[1], key = run.pc[2];
            run.pc += 3;
            mods_press(mods);
            if (key) keystate_press(KB_SRC_MATRIX, key);
            run.tap_mods = mods;
            run.tap_key  = key;
            run.state    = MS_FLUSH;
            run.started  = now;
            return;
        }
        if (op == MOP_DOWN) {
            uint8_t mods = run.pc[1], key = run.pc[2];
            run.pc += 3;
            mods_press(mods);
            run.held_mods = (uint8_t)(run.held_mods | mods);
            if (key) { keystate_press(KB_SRC_MATRIX, key); note_held(key); }
            continue;
        }
        if (op == MOP_UP) {
            uint8_t mods = run.pc[1], key = run.pc[2];
            run.pc += 3;
            if (key) { keystate_release(KB_SRC_MATRIX, key); drop_held(key); }
            mods_release(mods);
            run.held_mods = (uint8_t)(run.held_mods & ~mods);
            continue;
        }
        if (op == MOP_DELAY) {
            uint16_t ms = (uint16_t)(run.pc[1] | (run.pc[2] << 8));
            run.pc += 3;
            run.wake  = now + ms;
            run.state = MS_DELAY;
            return;
        }
        if (op == MOP_TEXT) {
            uint8_t n = run.pc[1];
            const char *t = (const char *)(run.pc + 2);
            run.pc += 2 + n;
            hid_push_type_string(t, n, 0);
            run.state   = MS_TEXT;
            run.started = now;
            return;
        }
        /* Unreachable: verify() rejects unknown opcodes. */
        finish();
        return;
    }
    finish();
}

void kb_macros_init(void) {
    kb_macro_store_init();
    memset(&run, 0, sizeof(run));
    run.state = MS_IDLE;
}

bool kb_macros_process(keyrecord_t *rec) {
    if (!kc_in(rec->keycode, QK_MACRO, QK_MACRO_MAX)) return true;

    uint8_t id = (uint8_t)(rec->keycode & 0xFF);
    if (rec->event.pressed && macro_start(id)) return false;
    if (rec->event.pressed || run.state == MS_IDLE) return kb_macro_user(id, rec);
    return false;
}

void kb_macros_task(void) {
    if (run.state == MS_IDLE) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    switch (run.state) {
    case MS_DELAY:
        if ((int32_t)(now - run.wake) < 0) return;
        run.state = MS_RUN;
        break;

    case MS_FLUSH:
        /* keystate_dirty() clears once hid_task() has composed and sent. */
        if (keystate_dirty() && (now - run.started) < MACRO_STEP_TIMEOUT_MS) return;
        if (run.tap_key) keystate_release(KB_SRC_MATRIX, run.tap_key);
        mods_release(run.tap_mods);
        run.state = MS_RUN;
        break;

    case MS_TEXT:
        if (hid_typer_busy() && (now - run.started) < MACRO_STEP_TIMEOUT_MS * 20) return;
        run.state = MS_RUN;
        break;

    default:
        break;
    }

    if (run.state == MS_RUN) macro_step(now);
}

#else   /* no macro store — KB_MACRO(n) resolves to compiled C only */

void kb_macros_init(void) { }

bool kb_macros_process(keyrecord_t *rec) {
    if (kc_in(rec->keycode, QK_MACRO, QK_MACRO_MAX))
        return kb_macro_user((uint8_t)(rec->keycode & 0xFF), rec);
    return true;
}

void kb_macros_task(void) { }

#endif
