#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "kb/kb.h"
#include "kb/keystate.h"
#if KB_FEATURE_MOUSEKEYS
#include "kb/mousekeys.h"
#endif
#if KB_FEATURE_DYNAMIC_KEYMAP && NUM_ENCODERS > 0
    // Encoder maps share the keymap's flash blob, so a save must persist both
    // and a reboot must bring both back. Storing them separately would mean two
    // Save buttons and a way to persist half your changes.
    { kb_encoder_set(0, 0, 1, KC_VOLU);
      kb_keymap_set(0, 0, 0, KC_Z);
      kb_keymap_save_request(); kb_keymap_commit_poll();
      kb_keymap_store_init();
      bool ok = kb_encoder_at(0, 0, 1) == KC_VOLU && kb_keymap_at(0, 0, 0) == KC_Z;
      printf("%-38s %s\n", "encoder map persists with the keymap", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }
#endif

#if KB_FEATURE_MACRO_STORE
#include "kb/macro_store.h"
#endif
#if KB_FEATURE_DYNAMIC_KEYMAP
#include "kb/keymap_store.h"
#include "hardware/flash.h"
extern uint8_t fake_flash[];
/* The keymap owns the last sector; the macro store owns the one below it. Poke
 * through this rather than absolute offsets, or adding another store silently
 * relocates what these tests are corrupting. */
#define KM_SECTOR (fake_flash + PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#endif

extern uint32_t fake_now_ms;
extern bool     fake_sw[2][2];

extern void    mouse_reset(void);
extern int     mouse_count(void);
extern int8_t  mouse_x(int);
extern int8_t  mouse_y(int);
extern int8_t  mouse_wheel(int);
extern uint8_t mouse_buttons(int);

static int failures = 0;

// Run the pipeline for `ms` milliseconds, printing every report the host
// would actually receive.
static void run(uint32_t ms, char *log, size_t logsz) {
    for (uint32_t i = 0; i < ms; i++) {
        kb_task();
        if (keystate_dirty()) {
            hid_keyboard_report_t r;
            if (keystate_compose(&r)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "[%02X %02X %02X] ",
                         r.modifier, r.keycode[0], r.keycode[1]);
                strncat(log, buf, logsz - strlen(log) - 1);
            }
        }
        fake_now_ms++;
    }
}

static void press(int r, int c)   { fake_sw[r][c] = true;  }
static void release(int r, int c) { fake_sw[r][c] = false; }

static void check(const char *name, const char *got, const char *want) {
    bool ok = strcmp(got, want) == 0;
    printf("%-38s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) { printf("    got : %s\n    want: %s\n", got, want); failures++; }
}

#define LOG char log[512] = ""

int main(void) {
    kb_init();
    LOG; run(20, log, sizeof(log));   // settle

    // 1. tap the layer-tap key: expect 'a' (0x04) down then up
    { LOG;
      press(0,0);   run(30, log, sizeof(log));
      release(0,0); run(40, log, sizeof(log));
      check("LT tap -> 'a'", log, "[00 04 00] [00 00 00] "); }

    // 2. hold the layer-tap key past TAPPING_TERM, then the bottom-right key,
    //    which is LSFT(KC_4) on layer 1 -> '$'
    { LOG;
      press(0,0);   run(260, log, sizeof(log));
      press(1,1);   run(20,  log, sizeof(log));
      release(1,1); run(20,  log, sizeof(log));
      release(0,0); run(20,  log, sizeof(log));
      check("LT hold + layer-1 key -> shift+4", log, "[02 21 00] [00 00 00] "); }

    // 3. mod-tap held: shift stays down while another key is tapped
    { LOG;
      press(0,1);   run(260, log, sizeof(log));
      press(1,1);   run(20,  log, sizeof(log));   // KC_D on layer 0
      release(1,1); run(20,  log, sizeof(log));
      release(0,1); run(20,  log, sizeof(log));
      check("SFT_T hold + d -> shift+d", log, "[02 00 00] [02 07 00] [02 00 00] [00 00 00] "); }

    // 4. both top keys chorded -> Escape (0x29)
    { LOG;
      press(0,0); press(0,1); run(30, log, sizeof(log));
      release(0,0); release(0,1); run(30, log, sizeof(log));
      check("combo A+B -> Esc", log, "[00 29 00] [00 00 00] "); }

    // 5. one-shot ctrl, then d
    { LOG;
      press(1,0);   run(20, log, sizeof(log));
      release(1,0); run(20, log, sizeof(log));
      press(1,1);   run(20, log, sizeof(log));
      release(1,1); run(20, log, sizeof(log));
      check("OSM(ctrl) then d -> ctrl+d", log, "[01 00 00] [01 07 00] [00 00 00] "); }

    // 6. the merge itself: a network report must not release a held matrix key
    { LOG;
      press(1,1); run(10, log, sizeof(log));                 // physical 'd'
      uint8_t net[6] = { 0x1A, 0, 0, 0, 0, 0 };              // network 'w'
      keystate_set_report(KB_SRC_NET, 0, net);
      run(10, log, sizeof(log));
      keystate_clear(KB_SRC_NET);
      run(10, log, sizeof(log));
      release(1,1); run(20, log, sizeof(log));
      check("net key merges, doesn't clobber matrix",
            log, "[00 07 00] [00 07 1A] [00 07 00] [00 00 00] "); }

#if KB_FEATURE_DYNAMIC_KEYMAP
    // 7. a live keymap edit takes effect on the very next press
    { LOG;
      kb_keymap_set(0, 1, 1, KC_Z);            // bottom-right was KC_D
      press(1,1);   run(20, log, sizeof(log));
      release(1,1); run(20, log, sizeof(log));
      check("runtime keymap edit -> 'z'", log, "[00 1D 00] [00 00 00] "); }

    // 8. that edit survives a save + reboot
    { kb_keymap_save_request();
      kb_keymap_commit_poll();                 // core 0 does the flash write
      kb_keymap_store_init();                  // simulate a power cycle
      bool ok = kb_keymap_at(0,1,1) == KC_Z && !kb_keymap_dirty() && kb_keymap_stored();
      printf("%-38s %s\n", "edit persists across reboot", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }

    // 9a. a stored blob from a DIFFERENT compiled keymap must lose to the
    //     freshly flashed one, or editing keymap.cpp appears to do nothing
    { kb_keymap_set(0, 1, 1, KC_Z);
      kb_keymap_save_request(); kb_keymap_commit_poll();
      KM_SECTOR[16] ^= 0xFF;                   // header.base_id
      kb_keymap_store_init();
      bool ok = kb_keymap_at(0,1,1) == KC_D && !kb_keymap_stored();
      printf("%-38s %s\n", "stored blob from other keymap loses", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }

    // 9. a blob whose dimensions do not match the firmware is refused rather
    //    than reinterpreted as a keymap of the wrong shape
    { KM_SECTOR[6] = 99;                       // header.rows
      kb_keymap_store_init();
      bool ok = kb_keymap_at(0,1,1) == KC_D && !kb_keymap_stored();
      printf("%-38s %s\n", "mismatched stored blob rejected", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }
#endif

#if KB_FEATURE_MOUSEKEYS
    // 10. holding a direction produces repeating motion that accelerates
    { LOG; (void)log;
      kb_keymap_set(0, 1, 1, MS_RGHT);
      mouse_reset();
      press(1,1);   run(200, log, sizeof(log));
      int n = mouse_count();
      bool all_right = n > 0;
      for (int i = 0; i < n; i++) if (mouse_x(i) <= 0 || mouse_y(i) != 0) all_right = false;
      bool ramped = n >= 2 && mouse_x(n-1) > mouse_x(0);
      release(1,1); run(20, log, sizeof(log));
      int after = mouse_count();
      bool stopped = true;
      { LOG; run(60, log, sizeof(log)); stopped = mouse_count() == after; }
      bool ok = all_right && ramped && stopped && n >= 5;
      printf("%-38s %s\n", "MS_RGHT repeats, ramps, then stops", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    reports=%d first=%d last=%d stopped=%d\n",
                        n, mouse_x(0), mouse_x(n?n-1:0), stopped); failures++; } }

    // 11. a diagonal must not travel faster than an axis
    { LOG;
      kb_keymap_set(0, 1, 1, MS_RGHT);
      kb_keymap_set(0, 1, 0, MS_DOWN);          // was OSM(ctrl)
      mouse_reset();
      press(1,1); press(1,0); run(150, log, sizeof(log));
      // Only the reports where both axes are moving are diagonals. The first
      // report of the gesture is single-axis by design: both switches close in
      // the same scan, MS_DOWN dispatches first and takes the immediate
      // one-step nudge, and the second direction joins on the next interval.
      int n = mouse_count(), diag = 0;
      bool ok = true;
      for (int i = 0; i < n; i++) {
          int x = mouse_x(i), y = mouse_y(i);
          if (x <= 0 || y <= 0) continue;
          diag++;
          // |(x,y)| must not exceed the axis speed by more than a rounding step
          if (x*x + y*y > (MOUSEKEY_MAX_SPEED + 1) * (MOUSEKEY_MAX_SPEED + 1)) ok = false;
      }
      ok = ok && diag >= 3;
      release(1,1); release(1,0); run(20, log, sizeof(log));
      printf("%-38s %s\n", "diagonal is speed-normalised", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    reports=%d diagonal=%d\n", n, diag); failures++; } }

    // 12. buttons are state, and every motion report carries them
    { LOG;
      kb_keymap_set(0, 1, 0, MS_BTN1);
      kb_keymap_set(0, 1, 1, MS_RGHT);
      mouse_reset();
      press(1,0); run(20, log, sizeof(log));
      bool down_ok = mouse_count() >= 1 && mouse_buttons(0) == 0x01 && mouse_x(0) == 0;
      int mark = mouse_count();
      press(1,1); run(80, log, sizeof(log));     // drag: motion while held
      bool drag_ok = mouse_count() > mark;
      for (int i = mark; i < mouse_count(); i++) if (mouse_buttons(i) != 0x01) drag_ok = false;
      release(1,1); run(20, log, sizeof(log));
      int mark2 = mouse_count();
      release(1,0); run(20, log, sizeof(log));
      bool up_ok = mouse_count() > mark2 && mouse_buttons(mouse_count()-1) == 0x00;
      bool ok = down_ok && drag_ok && up_ok;
      printf("%-38s %s\n", "BTN1 holds through a drag", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    down=%d drag=%d up=%d\n", down_ok, drag_ok, up_ok); failures++; } }
#endif

#if KB_FEATURE_DYNAMIC_KEYMAP && NUM_ENCODERS > 0
    // Encoder maps share the keymap's flash blob, so a save must persist both
    // and a reboot must bring both back. Storing them separately would mean two
    // Save buttons and a way to persist half your changes.
    { kb_encoder_set(0, 0, 1, KC_VOLU);
      kb_keymap_set(0, 0, 0, KC_Z);
      kb_keymap_save_request(); kb_keymap_commit_poll();
      kb_keymap_store_init();
      bool ok = kb_encoder_at(0, 0, 1) == KC_VOLU && kb_keymap_at(0, 0, 0) == KC_Z;
      printf("%-38s %s\n", "encoder map persists with the keymap", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }
#endif

#if KB_FEATURE_MACRO_STORE
    // 13. a stored macro runs on keypress: tap ctrl+c must appear as a real
    //     press and a distinct release, not a single collapsed report
    { kb_macro_clear_all();
      const uint8_t prog[] = { MOP_TAP, 0x01, KC_C, MOP_END };
      bool set = kb_macro_set(0, prog, sizeof(prog));
      kb_keymap_set(0, 1, 1, KB_MACRO(0));
      LOG;
      press(1,1);   run(60, log, sizeof(log));
      release(1,1); run(40, log, sizeof(log));
      bool ok = set && strcmp(log, "[01 06 00] [00 00 00] ") == 0;
      printf("%-38s %s\n", "stored macro taps ctrl+c", ok ? "PASS" : "FAIL");
      if (!ok) printf("    got: %s\n", log);
      if (!ok) failures++; }

    // 14. hold / delay / release across steps, with the delay actually elapsing
    { const uint8_t prog[] = { MOP_DOWN, 0x02, 0,          // hold shift
                               MOP_TAP,  0x00, KC_A,       // tap a
                               MOP_DELAY, 50, 0,           // 50 ms
                               MOP_TAP,  0x00, KC_B,       // tap b
                               MOP_UP,   0x02, 0,          // release shift
                               MOP_END };
      kb_macro_set(0, prog, sizeof(prog));
      LOG;
      press(1,1);   run(30, log, sizeof(log));
      const char *mid = strdup(log);
      run(80, log, sizeof(log));
      release(1,1); run(20, log, sizeof(log));
      // Shift down for both letters, and B must arrive only after the delay.
      bool shift_held = strstr(log, "[02 04 00]") && strstr(log, "[02 05 00]");
      bool b_after    = !strstr(mid, "[02 05 00]");
      bool cleaned    = strstr(log, "[00 00 00]") != NULL;
      bool ok = shift_held && b_after && cleaned;
      printf("%-38s %s\n", "hold+delay+release sequencing", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    got: %s\n", log); failures++; } }

    // 15. an unbalanced hold must not strand a key down forever.
    //     The delay is load-bearing: without it the hold and the end-of-macro
    //     cleanup fall in the same scan, collapse into one composed report, and
    //     the test would pass without ever having observed the key go down.
    { const uint8_t prog[] = { MOP_DOWN, 0x01, KC_X, MOP_DELAY, 30, 0, MOP_END };
      kb_macro_set(0, prog, sizeof(prog));
      LOG;
      press(1,1);   run(20, log, sizeof(log));
      bool went_down = strstr(log, "[01 1B 00]") != NULL;      // ctrl + x
      run(60, log, sizeof(log));
      release(1,1); run(30, log, sizeof(log));
      const char *last = strrchr(log, '[');
      bool released = last && strncmp(last, "[00 00 00]", 10) == 0;
      bool ok = went_down && released;
      printf("%-38s %s\n", "unbalanced hold is cleaned up", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    down=%d released=%d got: %s\n", went_down, released, log);
                 failures++; } }

    // 16. malformed bytecode is refused rather than handed to the interpreter
    { const uint8_t no_end[]  = { MOP_TAP, 0, KC_A };            // no END
      const uint8_t trunc[]   = { MOP_TAP, 0, MOP_END };         // TAP short by one
      const uint8_t bad_op[]  = { 0x7F, MOP_END };               // unknown opcode
      const uint8_t bad_txt[] = { MOP_TEXT, 2, 0x01, 'a', MOP_END };  // control char
      bool ok = !kb_macro_set(1, no_end,  sizeof(no_end))
             && !kb_macro_set(1, trunc,   sizeof(trunc))
             && !kb_macro_set(1, bad_op,  sizeof(bad_op))
             && !kb_macro_set(1, bad_txt, sizeof(bad_txt));
      printf("%-38s %s\n", "malformed bytecode rejected", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }

    // 17. macros survive a save and a reboot
    { const uint8_t prog[] = { MOP_TAP, 0x08, KC_L, MOP_END };
      kb_macro_set(3, prog, sizeof(prog));
      kb_macro_save_request(); kb_macro_commit_poll();
      kb_macro_store_init();
      uint16_t len = 0;
      const uint8_t *b = kb_macro_body(3, &len);
      bool ok = b && len == sizeof(prog) && memcmp(b, prog, len) == 0 && kb_macro_stored();
      printf("%-38s %s\n", "macros persist across reboot", ok ? "PASS" : "FAIL");
      if (!ok) failures++; }
#endif

#if KB_FEATURE_DYNAMIC_KEYMAP
    // A web-UI keypress: a report with auto_release, exactly as /api/key sends.
    // The press MUST reach the host before the release clears it. Handling the
    // auto-release before the compose meant it never did — the on-screen
    // keyboard did nothing while the physical matrix kept working, because
    // physical keys never set that flag.
    { LOG;
      // NOTE ON WHAT THIS DOES AND DOES NOT COVER.
      //
      // kbtest fakes the HID layer, so hid_task() itself is not under test here
      // — this asserts the invariant hid_task() has to respect: a press must be
      // composed and sent BEFORE the auto-release clears it. The real ordering
      // lives in hid.cpp and is only exercised on hardware, which is how the
      // bug reached a board in the first place.
      uint8_t k[6] = { KC_H, 0, 0, 0, 0, 0 };
      keystate_set_report(KB_SRC_NET, 0, k);
      run(10, log, sizeof(log));                 // compose + send the press
      bool press_sent = strstr(log, "[00 0B 00]") != NULL;
      // The release may only happen once the press is no longer dirty.
      bool premature = keystate_dirty();
      keystate_clear(KB_SRC_NET);
      run(20, log, sizeof(log));
      bool ok = press_sent && !premature &&
                strcmp(log, "[00 0B 00] [00 00 00] ") == 0;
      printf("%-38s %s\n", "web keypress is sent, then released", ok ? "PASS" : "FAIL");
      if (!ok) { printf("    got: %s\n", log); failures++; } }
#endif

    printf("\n%s\n", failures ? "FAILURES" : "all green");
    return failures;
}
