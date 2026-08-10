#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── Auth result codes ─────────────────────────────────────────────────────────
typedef enum {
    AUTH_OK,
    AUTH_WRONG,
    AUTH_LOCKED,
    AUTH_DISABLED,
} auth_result_t;

// ── Initialise (call once before both servers start) ─────────────────────────
void auth_init(void);

// ── Nonce / challenge size ────────────────────────────────────────────────────
#define AUTH_NONCE_HEX  32   // 16 random bytes, lowercase hex (+ NUL = 33)

// ── Password check (legacy plaintext, multi-user) ─────────────────────────────
// `user` may be NULL/"" to mean the first configured user (back-compat with
// clients that send only a password). The password crosses the wire, so callers
// should gate this on auth_plaintext_allowed(). On AUTH_LOCKED, *retry_after_s
// is set to seconds remaining.
auth_result_t auth_authenticate(const char *user, const char *password,
                                uint32_t *retry_after_s);

// ── Challenge-response (HMAC-SHA256, password never transmitted) ──────────────
// Issue a one-time nonce (lowercase hex, AUTH_NONCE_HEX chars). Returns false
// only if the challenge table is momentarily full.
bool auth_make_challenge(char nonce_hex_out[AUTH_NONCE_HEX + 1]);

// Verify response == HMAC-SHA256(key=user's password, msg=nonce_hex). The nonce
// must be one this device issued, unexpired and unused (single-use). `user` may
// be NULL/"" for the first user. Constant-time comparison.
auth_result_t auth_respond(const char *user, const char *nonce_hex,
                           const char *response_hex, uint32_t *retry_after_s);

// Whether legacy plaintext-password auth is permitted (ALLOW_PLAINTEXT_AUTH).
bool auth_plaintext_allowed(void);

// ── Session state ─────────────────────────────────────────────────────────────
bool     auth_is_enabled(void);      // false when PASSWORD == ""
bool     auth_is_multiuser(void);    // true when USER_LIST has more than one user
bool     auth_check(void);           // true if authenticated and not timed out
void     auth_touch(void);           // reset inactivity timer
void     auth_logout(void);          // explicitly lock

// ── Users ─────────────────────────────────────────────────────────────────────
// Resolve a configured user name to its index (NULL/"" → first user, index 0;
// unknown → -1), and the reverse (out-of-range → ""). Names point into const
// firmware storage and are stable for the program's lifetime.
int         auth_user_index(const char *user);
const char *auth_user_name(int idx);

// ── Web session tokens ────────────────────────────────────────────────────────
// create_token: allocates a random 32-hex-char token bound to `user`, returns it
// in buf[33]. `user` may be NULL/"" (→ first user).
void auth_create_token(const char *user, char *buf);
bool auth_validate_token(const char *token);   // also calls auth_touch()
void auth_invalidate_token(const char *token);
// Index of the user a (valid) token belongs to, or -1 if unknown/invalid.
int  auth_token_user_index(const char *token);

// Drop every session belonging to `user_idx`, except the one holding `keep`
// (pass NULL to drop all). Returns how many were dropped. Called when a
// password changes: a session opened with the old password must not survive it,
// but signing the person making the change straight back out is hostile.
int  auth_invalidate_user_tokens(int user_idx, const char *keep);

// ── Status ────────────────────────────────────────────────────────────────────
typedef struct {
    bool     authenticated;
    bool     locked;
    uint32_t locked_for_s;
    uint32_t idle_for_s;
    uint32_t timeout_s;
} auth_status_t;

void auth_get_status(auth_status_t *out);
