/* auth_store.cpp — see auth_store.h */

#include "auth_store.h"

#if ENABLE_PASSWORD_STORE

#include "config.h"
#include "nethid_build_id.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#define AS_MAGIC   0x5750484Eu     /* "NHPW" */
#define AS_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  count;            /* users covered; <= AUTH_STORE_MAX_USERS */
    uint8_t  reserved;
    uint32_t build_id;         /* the firmware that wrote this */
    uint32_t crc;              /* over `set` + `pw` */
    uint8_t  set[AUTH_STORE_MAX_USERS];
    char     pw[AUTH_STORE_MAX_USERS][AUTH_PW_MAX + 1];
} as_blob_t;

#define AS_PROG_LEN (((int)sizeof(as_blob_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static_assert(AS_PROG_LEN <= FLASH_SECTOR_SIZE,
              "password blob does not fit one sector - lower AUTH_STORE_MAX_USERS");

/* Sixth sector from the end. keymap 1, macros 2, wifi 3, settings 4,
 * autoclick 5. Keep docs/AUTH.md's map in step when adding another. */
#define AS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 6 * FLASH_SECTOR_SIZE)

static uint8_t      pw_set[AUTH_STORE_MAX_USERS];
static char         pw[AUTH_STORE_MAX_USERS][AUTH_PW_MAX + 1];
static volatile bool save_pending;
static bool          stored;      /* flash holds a record we accepted */

/* Running CRC32, so several buffers can be folded into ONE checksum rather than
 * checksummed separately and combined. Combining with XOR — which this did —
 * is worse than it looks: XOR is commutative, so it cannot tell the two regions
 * apart (the comment claiming it caught a transposition was simply wrong), and
 * any pair of errors whose CRC deltas match cancels out entirely. */
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c;
}

static uint32_t blob_crc(const as_blob_t *b) {
    /* Both arrays through one running CRC, in a fixed order. Not one call over
     * the struct: `set` and `pw` are adjacent today, but that is a layout
     * accident and any padding between them would be uninitialised bytes in the
     * checksum — a record that fails its own CRC on the next boot, for nobody's
     * benefit. */
    uint32_t c = crc32_update(0xFFFFFFFFu, b->set, sizeof(b->set));
    c = crc32_update(c, (const uint8_t *)b->pw, sizeof(b->pw));
    return ~c;
}

static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + AS_FLASH_OFFSET);
    as_blob_t b;
    memcpy(&b, base, sizeof(b));

    if (b.magic != AS_MAGIC || b.version != AS_VERSION) return false;
    if (b.count > AUTH_STORE_MAX_USERS) return false;

    /* The whole point of this store. A record written by a different image is
     * not corrupt and is not wrong — it simply does not apply, because the
     * firmware it belonged to has been replaced. Say so plainly: this line is
     * the one that tells someone locked out that reflashing worked. */
    if (b.build_id != NETHID_BUILD_ID) {
        printf("[auth-store] stored password was written by build %08lx, this is %s"
               " - ignoring it and using the compiled password\n",
               (unsigned long)b.build_id, NETHID_BUILD_ID_STR);
        return false;
    }

    if (blob_crc(&b) != b.crc) {
        printf("[auth-store] stored password failed CRC - ignoring\n");
        return false;
    }

    memcpy(pw_set, b.set, sizeof(pw_set));
    memcpy(pw, b.pw, sizeof(pw));
    /* Terminate defensively: a corrupt-but-CRC-valid blob must never hand an
     * unterminated string to strcmp() or to the HMAC. */
    for (int i = 0; i < AUTH_STORE_MAX_USERS; i++) pw[i][AUTH_PW_MAX] = '\0';

    int n = 0;
    for (int i = 0; i < AUTH_STORE_MAX_USERS; i++) if (pw_set[i]) n++;
    printf("[auth-store] using %d stored password(s)\n", n);
    return true;
}

static void __no_inline_not_in_flash_func(as_flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(AS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(AS_FLASH_OFFSET, blob, AS_PROG_LEN);
    restore_interrupts(ints);
}

void auth_store_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    static uint8_t raw[AS_PROG_LEN];
    memset(raw, 0xFF, sizeof(raw));

    as_blob_t b = {};
    b.magic    = AS_MAGIC;
    b.version  = AS_VERSION;
    b.count    = AUTH_STORE_MAX_USERS;
    b.build_id = NETHID_BUILD_ID;
    memcpy(b.set, pw_set, sizeof(b.set));
    memcpy(b.pw,  pw,     sizeof(b.pw));
    b.crc      = blob_crc(&b);
    memcpy(raw, &b, sizeof(b));

    printf("[auth-store] committing to flash\n");
    multicore_lockout_start_blocking();
    as_flash_commit(raw);
    multicore_lockout_end_blocking();
    stored = auth_store_any();

    /* Wipe the staging copy: it is the only place the plaintext sits in RAM
     * outside the live table, and it is static so it would otherwise persist
     * for the life of the program. */
    memset(raw, 0xFF, sizeof(raw));
    memset(&b, 0, sizeof(b));

    printf("[auth-store] saved\n");
}

void auth_store_init(void) {
    memset(pw_set, 0, sizeof(pw_set));
    memset(pw, 0, sizeof(pw));
    /* `&& auth_store_any()`, because a record that parsed cleanly but holds no
     * overrides is the same situation as no record at all, and must report the
     * same. Without it this said "stored" for a valid empty record while
     * commit_poll() said the opposite for the identical state — one flag, two
     * meanings, depending on whether you had just written it or just booted. */
    stored = load_from_flash() && auth_store_any();
    save_pending = false;
}

const char *auth_store_password(int idx) {
    if (idx < 0 || idx >= AUTH_STORE_MAX_USERS) return NULL;
    return pw_set[idx] ? pw[idx] : NULL;
}

bool auth_store_set(int idx, const char *password) {
    if (idx < 0 || idx >= AUTH_STORE_MAX_USERS) return false;

    if (!password || !password[0]) {          /* clear -> back to compiled */
        pw_set[idx] = 0;
        memset(pw[idx], 0, sizeof(pw[idx]));
        save_pending = true;
        return true;
    }
    if (strlen(password) > AUTH_PW_MAX) return false;

    memset(pw[idx], 0, sizeof(pw[idx]));
    strncpy(pw[idx], password, AUTH_PW_MAX);
    pw[idx][AUTH_PW_MAX] = '\0';
    pw_set[idx] = 1;
    save_pending = true;
    return true;
}

bool auth_store_any(void) {
    for (int i = 0; i < AUTH_STORE_MAX_USERS; i++) if (pw_set[i]) return true;
    return false;
}

bool auth_store_stored(void)       { return stored; }
bool auth_store_save_pending(void) { return save_pending; }

uint32_t    auth_store_build_id(void)     { return NETHID_BUILD_ID; }
const char *auth_store_build_id_str(void) { return NETHID_BUILD_ID_STR; }

#else  /* password store compiled out */

/* No config.h and no SDK headers on this branch — it must not need them, since
 * the point of it is a build that has no web server. That also means no NULL. */
#include <stddef.h>

void        auth_store_init(void)                  {}
const char *auth_store_password(int)               { return nullptr; }
bool        auth_store_set(int, const char *)      { return false; }
bool        auth_store_any(void)                   { return false; }
bool        auth_store_stored(void)                { return false; }
bool        auth_store_save_pending(void)          { return false; }
void        auth_store_commit_poll(void)           {}
uint32_t    auth_store_build_id(void)              { return 0; }
const char *auth_store_build_id_str(void)          { return ""; }

#endif
