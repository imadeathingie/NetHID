/* Host-side check of the settings store: defaults, ranges, override tracking,
 * persistence, and the field-count guard. */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "settings.h"
#ifndef TAPPING_TERM
#define TAPPING_TERM 200
#endif
#include "hardware/flash.h"
extern uint8_t fake_flash[];
#define ST_SECTOR (fake_flash + PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-40s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    memset(fake_flash, 0xFF, PICO_FLASH_SIZE_BYTES);
    settings_init();

    ck("defaults come from config.h",
       settings()->tapping_term_ms == TAPPING_TERM &&
       settings()->quiet_boot == QUIET_BOOT);

    ck("out-of-range is rejected, not clamped",
       !settings_set("tapping_term_ms", 5000) &&
       settings()->tapping_term_ms == TAPPING_TERM);

    ck("unknown field is rejected", !settings_set("nope", 1));

    ck("a set value applies and is marked overridden",
       settings_set("tapping_term_ms", 175) &&
       settings()->tapping_term_ms == 175 &&
       settings_is_overridden("tapping_term_ms") &&
       !settings_is_overridden("quiet_boot"));

    settings_save_request();
    settings_commit_poll();
    settings_init();
    ck("overrides survive a reboot",
       settings()->tapping_term_ms == 175 && settings_is_overridden("tapping_term_ms"));

    // The point of the mask: an untouched field must still track config.h, so a
    // later rebuild moves it rather than freezing it at save time.
    ck("untouched fields still track config.h",
       !settings_is_overridden("lockout_s") && settings()->lockout_s == LOCKOUT_S);

    ck("reset returns a field to its default",
       settings_reset("tapping_term_ms") &&
       settings()->tapping_term_ms == TAPPING_TERM &&
       !settings_is_overridden("tapping_term_ms"));

    // A firmware that adds a field must not reinterpret the old blob.
    settings_set("quiet_boot", 1);
    settings_save_request(); settings_commit_poll();
    ST_SECTOR[6] = 99;                         // header.nfields
    settings_init();
    ck("field-count mismatch falls back to defaults",
       settings()->quiet_boot == QUIET_BOOT && !settings_is_overridden("quiet_boot"));

    // ── Boot-key overrides ──────────────────────────────────────────────────
    // These are gestures about this boot. They must not write to flash, and
    // must not show up on the settings page as if someone had saved them.
    memset(fake_flash, 0xFF, PICO_FLASH_SIZE_BYTES);
    settings_init();
    ck("default is not quiet",
       !settings_quiet_boot_effective() && settings()->quiet_boot == QUIET_BOOT);

    settings_force_quiet_boot();
    ck("quiet-boot key silences this boot only",
       settings_quiet_boot_effective() &&
       settings()->quiet_boot == QUIET_BOOT &&      /* stored value untouched */
       !settings_is_overridden("quiet_boot"));

    // The case the loud key exists for: quiet_boot saved to flash, so the device
    // says nothing on every boot and you need a way back in without a reflash.
    settings_init();
    settings_set("quiet_boot", 1);
    settings_save_request(); settings_commit_poll();
    settings_init();
    ck("stored quiet_boot silences boot",
       settings_quiet_boot_effective() && settings_is_overridden("quiet_boot"));

    settings_force_loud_boot();
    ck("loud-boot key overrides stored quiet_boot",
       !settings_quiet_boot_effective() &&
       settings()->quiet_boot == 1 &&               /* still saved, just overridden */
       settings_is_overridden("quiet_boot"));

    // Both held: loud wins. Between "say nothing" and "say something", the
    // recoverable choice has to be the one that happens.
    settings_init();
    settings_force_quiet_boot();
    settings_force_loud_boot();
    ck("loud beats quiet when both keys are held",
       !settings_quiet_boot_effective());

    char jb[2048];
    int n = settings_to_json(jb, sizeof(jb));
    ck("JSON fits and is well formed",
       n > 0 && (size_t)n < sizeof(jb) && jb[0] == '{' && jb[n-1] == '}' &&
       strstr(jb, "\"tapping_term_ms\"") && strstr(jb, "\"help\""));

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
