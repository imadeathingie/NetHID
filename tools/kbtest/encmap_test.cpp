/*
 * encmap_test — encoder maps persist with the keymap.
 *
 * Built against a board that HAS an encoder (oledpad), because the main kbtest
 * uses proto2x2 where NUM_ENCODERS is 0 and the assertion compiles away to
 * nothing. A test that silently does not run is worse than no test: it reports
 * PASS.
 *
 * The property: encoder maps share the keymap's flash blob and its dirty flag,
 * so one Save persists both. Separate stores would mean two Save buttons and a
 * way to persist half your changes.
 */
#include <stdio.h>
#include <string.h>
#include "kb/kb.h"
#include "kb/keymap_store.h"
#include "hardware/flash.h"
extern uint8_t fake_flash[];
int main(void) {
    memset(fake_flash, 0xFF, PICO_FLASH_SIZE_BYTES);
    kb_keymap_store_init();
    printf("encoders reported: %u\n", kb_encoder_count());
    bool d0 = kb_encoder_at(0, 0, 1) != 0;          /* compiled default present */
    kb_encoder_set(0, 0, 1, KC_VOLU);
    kb_keymap_set(0, 0, 0, KC_Z);
    kb_keymap_save_request(); kb_keymap_commit_poll();
    kb_keymap_store_init();
    bool ok = kb_encoder_count() == NUM_ENCODERS && d0 &&
              kb_encoder_at(0, 0, 1) == KC_VOLU && kb_keymap_at(0, 0, 0) == KC_Z;
    printf("%-46s %s\n", "encoder map persists with the keymap", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
void kb_macro_string(const char*) {}
