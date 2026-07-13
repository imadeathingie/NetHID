/*
 * auth.cpp — NetHID authentication state machine
 *
 * One global authenticated flag with an inactivity timer.
 * Web sessions use random 32-hex-char tokens stored in a small fixed table.
 * Brute-force protection: lock out for LOCKOUT_S after MAX_AUTH_ATTEMPTS
 * consecutive wrong passwords.
 *
 * Thread safety: all state is protected by a single Pico SDK spin-lock so
 * it is safe to call from both the lwIP IRQ context (core 0 background) and
 * from core 0 directly.
 */

#include "auth.h"
#include "config.h"
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "pico/rand.h"
#include "hmac_sha256.h"
#include <string.h>
#include <stdio.h>

// ── User table (from env.h USER_LIST) ─────────────────────────────────────────
typedef struct { const char *user; const char *password; } nethid_user_t;

#define USER(u, p) { (u), (p) },
static const nethid_user_t USERS[] = {
    USER_LIST
    { NULL, NULL }   // sentinel keeps the array non-empty; excluded from count
};
#undef USER
static const int USER_N = (int)(sizeof(USERS) / sizeof(USERS[0])) - 1;

bool auth_is_multiuser(void) { return USER_N > 1; }

// Resolve a username to its entry. NULL/"" -> first user (legacy single-pw).
static const nethid_user_t *find_user(const char *user) {
    if (!user || user[0] == '\0') return (USER_N > 0) ? &USERS[0] : NULL;
    for (int i = 0; i < USER_N; ++i)
        if (strcmp(USERS[i].user, user) == 0) return &USERS[i];
    return NULL;
}

int auth_user_index(const char *user) {
    const nethid_user_t *u = find_user(user);
    if (!u) return -1;
    return (int)(u - USERS);              // position in USERS[]
}

const char *auth_user_name(int idx) {
    if (idx < 0 || idx >= USER_N) return "";
    return USERS[idx].user;
}

// Constant-time compare of two NUL-terminated hex strings of expected length n.
static bool ct_equal_hex(const char *a, const char *b, int n) {
    unsigned char diff = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char bc = (unsigned char)b[i];   // stops at b's NUL -> 0
        diff |= (unsigned char)((unsigned char)a[i] ^ bc);
        if (bc == 0) diff |= 1;                    // b shorter than n
    }
    if (b[n] != '\0') diff |= 1;                   // b longer than n
    return diff == 0;
}

// ── Challenge table (one-time nonces for HMAC auth) ───────────────────────────
#define MAX_CHALLENGES   8
#define CHALLENGE_TTL_MS 60000   // 60 s to answer a challenge

typedef struct {
    char     nonce[AUTH_NONCE_HEX + 1];
    uint32_t expiry_ms;
    bool     active;             // issued and not yet consumed
} challenge_t;

// ── Token table ───────────────────────────────────────────────────────────────
#define MAX_TOKENS      8
#define TOKEN_LEN       32   // hex chars (128-bit random)

typedef struct {
    char     token[TOKEN_LEN + 1];
    uint32_t expiry_ms;   // absolute_time equivalent in ms since boot
    bool     used;
    int      user_idx;    // which configured user this session belongs to
} web_token_t;

// ── State ─────────────────────────────────────────────────────────────────────
static struct {
    bool         authenticated;
    uint32_t     last_activity_ms;
    uint32_t     failed_attempts;
    uint32_t     locked_until_ms;  // 0 = not locked
    web_token_t  tokens[MAX_TOKENS];
    challenge_t  challenges[MAX_CHALLENGES];
    spin_lock_t *lock;
} s;

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline uint32_t now_ms(void) {
    return (uint32_t)(to_ms_since_boot(get_absolute_time()));
}

static void _expire(void) {
    s.authenticated    = false;
    s.last_activity_ms = 0;
    for (int i = 0; i < MAX_TOKENS; i++) s.tokens[i].used = false;
}

static void _prune_tokens(void) {
    uint32_t n = now_ms();
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (s.tokens[i].used && (int32_t)(n - s.tokens[i].expiry_ms) >= 0)
            s.tokens[i].used = false;
    }
}

static void _hex_token(char *buf) {
    // Generate 16 random bytes → 32 hex chars
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)(get_rand_32() & 0xFF);
        buf[i*2]     = hex[b >> 4];
        buf[i*2 + 1] = hex[b & 0xF];
    }
    buf[32] = '\0';
}

// ── Public API ────────────────────────────────────────────────────────────────

void auth_init(void) {
    memset(&s, 0, sizeof(s));
    int lock_num = spin_lock_claim_unused(true);
    s.lock = spin_lock_instance(lock_num);
}

bool auth_is_enabled(void) {
    // Enabled if any configured user has a non-empty password.
    for (int i = 0; i < USER_N; ++i)
        if (USERS[i].password && USERS[i].password[0] != '\0') return true;
    return false;
}

bool auth_plaintext_allowed(void) {
    return ALLOW_PLAINTEXT_AUTH ? true : false;
}

bool auth_check(void) {
    if (!auth_is_enabled()) return true;
    uint32_t irq = spin_lock_blocking(s.lock);
    bool ok = s.authenticated;
    if (ok) {
        uint32_t idle = now_ms() - s.last_activity_ms;
        if (idle > (uint32_t)SESSION_TIMEOUT_S * 1000) {
            _expire();
            ok = false;
        }
    }
    spin_unlock(s.lock, irq);
    return ok;
}

void auth_touch(void) {
    uint32_t irq = spin_lock_blocking(s.lock);
    s.last_activity_ms = now_ms();
    spin_unlock(s.lock, irq);
}

void auth_logout(void) {
    uint32_t irq = spin_lock_blocking(s.lock);
    _expire();
    spin_unlock(s.lock, irq);
    printf("[auth] Logged out\n");
}

// Shared lockout check. Call with the lock held. Returns true (and fills
// *retry) if currently locked out.
static bool _locked(uint32_t n, uint32_t *retry) {
    if (s.locked_until_ms && (int32_t)(s.locked_until_ms - n) > 0) {
        if (retry) *retry = (s.locked_until_ms - n + 999) / 1000;
        return true;
    }
    return false;
}

// Shared "authentication succeeded" / "failed" bookkeeping. Call with lock held.
static void _on_success(uint32_t n) {
    s.authenticated    = true;
    s.last_activity_ms = n;
    s.failed_attempts  = 0;
    s.locked_until_ms  = 0;
}
static auth_result_t _on_failure(uint32_t n, uint32_t *retry) {
    s.failed_attempts++;
    printf("[auth] Auth failed (%u/%u)\n",
           (unsigned)s.failed_attempts, (unsigned)MAX_AUTH_ATTEMPTS);
    if (s.failed_attempts >= MAX_AUTH_ATTEMPTS) {
        s.locked_until_ms = n + (uint32_t)LOCKOUT_S * 1000;
        s.failed_attempts = 0;
        if (retry) *retry = LOCKOUT_S;
        printf("[auth] Locked for %d s\n", LOCKOUT_S);
        return AUTH_LOCKED;
    }
    return AUTH_WRONG;
}

auth_result_t auth_authenticate(const char *user, const char *password,
                                uint32_t *retry_after_s) {
    if (retry_after_s) *retry_after_s = 0;
    if (!auth_is_enabled()) return AUTH_DISABLED;
    if (!password) password = "";

    uint32_t irq = spin_lock_blocking(s.lock);
    uint32_t n   = now_ms();

    if (_locked(n, retry_after_s)) {
        spin_unlock(s.lock, irq);
        return AUTH_LOCKED;
    }

    const nethid_user_t *u = find_user(user);
    if (u && u->password[0] != '\0' && strcmp(password, u->password) == 0) {
        _on_success(n);
        spin_unlock(s.lock, irq);
        printf("[auth] Authenticated (plaintext) as '%s'\n", u->user);
        return AUTH_OK;
    }

    auth_result_t r = _on_failure(n, retry_after_s);
    spin_unlock(s.lock, irq);
    return r;
}

// ── Challenge-response (HMAC) ─────────────────────────────────────────────────

static void _prune_challenges(uint32_t n) {
    for (int i = 0; i < MAX_CHALLENGES; ++i)
        if (s.challenges[i].active && (int32_t)(n - s.challenges[i].expiry_ms) >= 0)
            s.challenges[i].active = false;
}

bool auth_make_challenge(char nonce_hex_out[AUTH_NONCE_HEX + 1]) {
    static const char hex[] = "0123456789abcdef";
    uint32_t irq = spin_lock_blocking(s.lock);
    uint32_t n   = now_ms();
    _prune_challenges(n);

    int slot = -1;
    uint32_t oldest = UINT32_MAX; int oldest_slot = 0;
    for (int i = 0; i < MAX_CHALLENGES; ++i) {
        if (!s.challenges[i].active) { slot = i; break; }
        if (s.challenges[i].expiry_ms < oldest) { oldest = s.challenges[i].expiry_ms; oldest_slot = i; }
    }
    if (slot < 0) slot = oldest_slot;   // evict the oldest if all in use

    for (int i = 0; i < 16; ++i) {
        uint8_t b = (uint8_t)(get_rand_32() & 0xFF);
        s.challenges[slot].nonce[i*2]     = hex[b >> 4];
        s.challenges[slot].nonce[i*2 + 1] = hex[b & 0xF];
    }
    s.challenges[slot].nonce[AUTH_NONCE_HEX] = '\0';
    s.challenges[slot].expiry_ms = n + CHALLENGE_TTL_MS;
    s.challenges[slot].active    = true;

    memcpy(nonce_hex_out, s.challenges[slot].nonce, AUTH_NONCE_HEX + 1);
    spin_unlock(s.lock, irq);
    return true;
}

// Find a live challenge matching nonce_hex and CONSUME it (single-use). Returns
// true if found+consumed. Call with lock held.
static bool _consume_challenge(const char *nonce_hex, uint32_t n) {
    for (int i = 0; i < MAX_CHALLENGES; ++i) {
        if (!s.challenges[i].active) continue;
        if ((int32_t)(n - s.challenges[i].expiry_ms) >= 0) { s.challenges[i].active = false; continue; }
        if (strcmp(s.challenges[i].nonce, nonce_hex) == 0) {
            s.challenges[i].active = false;   // consume
            return true;
        }
    }
    return false;
}

auth_result_t auth_respond(const char *user, const char *nonce_hex,
                           const char *response_hex, uint32_t *retry_after_s) {
    if (retry_after_s) *retry_after_s = 0;
    if (!auth_is_enabled()) return AUTH_DISABLED;
    if (!nonce_hex || !response_hex) return AUTH_WRONG;

    uint32_t irq = spin_lock_blocking(s.lock);
    uint32_t n   = now_ms();

    if (_locked(n, retry_after_s)) {
        spin_unlock(s.lock, irq);
        return AUTH_LOCKED;
    }

    bool nonce_ok = _consume_challenge(nonce_hex, n);   // single-use, even on fail
    const nethid_user_t *u = find_user(user);

    bool ok = false;
    if (nonce_ok && u && u->password[0] != '\0') {
        char expected[65];
        hmac_sha256_hex((const uint8_t *)u->password, strlen(u->password),
                        (const uint8_t *)nonce_hex, strlen(nonce_hex), expected);
        ok = ct_equal_hex(expected, response_hex, 64);
    }

    if (ok) {
        _on_success(n);
        spin_unlock(s.lock, irq);
        printf("[auth] Authenticated (hmac) as '%s'\n", u->user);
        return AUTH_OK;
    }

    auth_result_t r = _on_failure(n, retry_after_s);
    spin_unlock(s.lock, irq);
    return r;
}

void auth_create_token(const char *user, char *buf) {
    int uidx = auth_user_index(user);
    if (uidx < 0) uidx = 0;               // unknown -> first user (defensive)
    uint32_t irq = spin_lock_blocking(s.lock);
    _prune_tokens();

    // Find a free slot; if full evict the oldest
    int slot = -1;
    uint32_t oldest_exp = UINT32_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!s.tokens[i].used) { slot = i; break; }
        if (s.tokens[i].expiry_ms < oldest_exp) {
            oldest_exp  = s.tokens[i].expiry_ms;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;

    _hex_token(s.tokens[slot].token);
    s.tokens[slot].expiry_ms = now_ms() + (uint32_t)SESSION_TIMEOUT_S * 1000;
    s.tokens[slot].used      = true;
    s.tokens[slot].user_idx  = uidx;

    memcpy(buf, s.tokens[slot].token, TOKEN_LEN + 1);
    spin_unlock(s.lock, irq);
}

int auth_token_user_index(const char *token) {
    if (!token || token[0] == '\0') return -1;
    uint32_t irq = spin_lock_blocking(s.lock);
    int idx = -1;
    if (s.authenticated) {
        for (int i = 0; i < MAX_TOKENS; i++) {
            if (s.tokens[i].used && strcmp(s.tokens[i].token, token) == 0) {
                idx = s.tokens[i].user_idx;
                break;
            }
        }
    }
    spin_unlock(s.lock, irq);
    return idx;
}

bool auth_validate_token(const char *token) {
    if (!token || token[0] == '\0') return false;

    uint32_t irq = spin_lock_blocking(s.lock);
    uint32_t n   = now_ms();

    // First check global session
    if (!s.authenticated ||
        (int32_t)(n - s.last_activity_ms) > (int32_t)SESSION_TIMEOUT_S * 1000) {
        _expire();
        spin_unlock(s.lock, irq);
        return false;
    }

    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!s.tokens[i].used) continue;
        if (strcmp(s.tokens[i].token, token) != 0) continue;
        if ((int32_t)(n - s.tokens[i].expiry_ms) >= 0) {
            s.tokens[i].used = false;
            spin_unlock(s.lock, irq);
            return false;
        }
        // Refresh token and global timer
        s.tokens[i].expiry_ms = n + (uint32_t)SESSION_TIMEOUT_S * 1000;
        s.last_activity_ms    = n;
        spin_unlock(s.lock, irq);
        return true;
    }

    spin_unlock(s.lock, irq);
    return false;
}

void auth_invalidate_token(const char *token) {
    if (!token || token[0] == '\0') return;
    uint32_t irq = spin_lock_blocking(s.lock);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (s.tokens[i].used && strcmp(s.tokens[i].token, token) == 0) {
            s.tokens[i].used = false;
            break;
        }
    }
    spin_unlock(s.lock, irq);
}

void auth_get_status(auth_status_t *out) {
    uint32_t irq = spin_lock_blocking(s.lock);
    uint32_t n   = now_ms();
    out->authenticated = s.authenticated;
    out->locked        = (s.locked_until_ms && (int32_t)(s.locked_until_ms - n) > 0);
    out->locked_for_s  = out->locked ? (s.locked_until_ms - n + 999) / 1000 : 0;
    out->idle_for_s    = s.authenticated ? (n - s.last_activity_ms) / 1000 : 0;
    out->timeout_s     = SESSION_TIMEOUT_S;
    spin_unlock(s.lock, irq);
}
