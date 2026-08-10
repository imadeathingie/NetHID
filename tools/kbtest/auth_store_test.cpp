/*
 * auth_store_test.cpp — the runtime password override, on the host.
 *
 * The property being tested is the one that makes the feature safe to ship:
 * a password stored by one firmware must NOT be honoured by the next one
 * flashed. If that ever regresses, someone who forgets a password they set from
 * the web UI has no way back into their own keyboard — the device has no reset
 * hole, and the flash sector survives a UF2 write.
 *
 * The stub nethid_build_id.h makes the build id a variable, so "flash a new
 * firmware" is `test_build_id = something else; auth_store_init();` with the
 * fake flash left exactly as it was.
 *
 *     make auth_store_test && ./auth_store_test
 */
#include "auth_store.h"
#include "hardware/flash.h"
#include <stdio.h>
#include <string.h>

uint32_t test_build_id = 0xAAAA0001u;

static int fails;
static void ok(bool c, const char *what) {
    printf("%s  %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}

// The store queues writes for core 0; on the host we just run the poll.
static void save(void) { auth_store_commit_poll(); }

static bool pw_is(int idx, const char *expect) {
    const char *p = auth_store_password(idx);
    if (!expect) return p == nullptr;
    return p && strcmp(p, expect) == 0;
}

int main(void) {
    memset(fake_flash, 0xFF, sizeof(fake_flash));

    // ── Nothing stored ──────────────────────────────────────────────────────
    auth_store_init();
    ok(pw_is(0, nullptr), "no override before anything is stored");
    ok(!auth_store_any(), "auth_store_any() false when empty");
    ok(!auth_store_stored(), "auth_store_stored() false on blank flash");

    // ── Set and persist ─────────────────────────────────────────────────────
    ok(auth_store_set(0, "hunter2000"), "setting a password is accepted");
    ok(pw_is(0, "hunter2000"), "it takes effect immediately, before the write");
    ok(auth_store_save_pending(), "a write is queued");
    save();
    ok(!auth_store_save_pending(), "the queue is clear after the commit");
    ok(auth_store_stored(), "flash now holds a record");

    // ── Reboot, same firmware ───────────────────────────────────────────────
    auth_store_init();
    ok(pw_is(0, "hunter2000"), "the password survives a reboot");
    ok(auth_store_stored(), "and is reported as stored");

    // ── THE ONE THAT MATTERS: flash a different firmware ────────────────────
    // Same flash contents, new build id. The record must be disowned.
    test_build_id = 0xBBBB0002u;
    auth_store_init();
    ok(pw_is(0, nullptr), "a new firmware ignores the previous one's password");
    ok(!auth_store_any(), "so nothing is overridden");
    ok(!auth_store_stored(), "and it does not claim to have a stored record");

    // Reflashing the SAME image again must not resurrect it either, because the
    // sector was never rewritten - it is still the old build's record.
    auth_store_init();
    ok(pw_is(0, nullptr), "still ignored on a second boot of the new firmware");

    // ── The new firmware can store its own ──────────────────────────────────
    auth_store_set(0, "newpassword");
    save();
    auth_store_init();
    ok(pw_is(0, "newpassword"), "the new firmware's own password persists");

    // ...and going back to the old build id disowns THAT one. The rule is
    // "written by the image that is running", not "newer wins".
    test_build_id = 0xAAAA0001u;
    auth_store_init();
    ok(pw_is(0, nullptr), "an older firmware does not inherit it either");
    test_build_id = 0xBBBB0002u;
    auth_store_init();
    ok(pw_is(0, "newpassword"), "and the owning firmware still has it");

    // ── Clearing ────────────────────────────────────────────────────────────
    ok(auth_store_set(0, nullptr), "clearing is accepted");
    ok(pw_is(0, nullptr), "cleared immediately");
    save();
    auth_store_init();
    ok(pw_is(0, nullptr), "and stays cleared across a reboot");

    // ── Several users ───────────────────────────────────────────────────────
    auth_store_set(0, "adminpass1");
    auth_store_set(2, "bobpassword");
    save();
    auth_store_init();
    ok(pw_is(0, "adminpass1"), "user 0 keeps its own password");
    ok(pw_is(1, nullptr),      "a user with no override still has none");
    ok(pw_is(2, "bobpassword"), "user 2 keeps its own password");
    // Overrides are per user; clearing one must not disturb another. They share
    // one blob, so this is a real risk rather than a theoretical one.
    auth_store_set(0, nullptr);
    save();
    auth_store_init();
    ok(pw_is(0, nullptr),       "clearing user 0 leaves it cleared");
    ok(pw_is(2, "bobpassword"), "and does not disturb user 2");

    // ── Bounds ──────────────────────────────────────────────────────────────
    char toolong[AUTH_PW_MAX + 8];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    ok(!auth_store_set(0, toolong), "an over-length password is refused");

    char exact[AUTH_PW_MAX + 1];
    memset(exact, 'y', AUTH_PW_MAX);
    exact[AUTH_PW_MAX] = '\0';
    ok(auth_store_set(1, exact), "one of exactly AUTH_PW_MAX is accepted");
    save();
    auth_store_init();
    ok(pw_is(1, exact), "and round-trips through flash intact");

    ok(auth_store_password(-1) == nullptr, "a negative index is not a password");
    ok(auth_store_password(AUTH_STORE_MAX_USERS) == nullptr,
       "nor is an index past the end");
    ok(!auth_store_set(AUTH_STORE_MAX_USERS, "whatever"),
       "and setting one past the end is refused");

    // ── Corruption ──────────────────────────────────────────────────────────
    // A bad CRC must fall back to the compiled password rather than handing the
    // HMAC a mangled key, which would lock everyone out with no explanation.
    auth_store_set(0, "goodpassword");
    save();
    auth_store_init();
    ok(pw_is(0, "goodpassword"), "stored before corrupting");
    // Flip a byte inside the password area of the sector.
    fake_flash[PICO_FLASH_SIZE_BYTES - 6 * FLASH_SECTOR_SIZE + 40] ^= 0xFF;
    auth_store_init();
    ok(pw_is(0, nullptr), "a corrupt record is ignored, not used");

    // ── Clearing every user reaches flash ───────────────────────────────────
    // The web UI reverts one user at a time (auth_store_set(i, NULL)); this is
    // the same path applied to all of them, which is the only "reset" there is.
    memset(fake_flash, 0xFF, sizeof(fake_flash));
    auth_store_init();
    auth_store_set(0, "temporary01");
    auth_store_set(3, "temporary02");
    save();
    for (int i = 0; i < AUTH_STORE_MAX_USERS; i++) auth_store_set(i, nullptr);
    save();
    ok(!auth_store_any(), "clearing every user takes effect");
    ok(!auth_store_stored(), "and stored() agrees, right after the write");
    auth_store_init();
    ok(!auth_store_any(), "still cleared after a reboot");
    // stored() used to disagree with itself here: commit_poll() reported false
    // for an empty table while init() reported true for the valid-but-empty
    // record it had just written. One flag, two meanings, depending on path.
    ok(!auth_store_stored(), "and stored() agrees after a reboot too");

    printf(fails ? "\n%d FAILED\n" : "\nall green\n", fails);
    return fails ? 1 : 0;
}
