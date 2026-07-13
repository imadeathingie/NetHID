#pragma once
// ============================================================
//  secrets.h — firmware-stored secrets, referenced in TEXT shortcuts as ${NAME}.
//
//  The values live in env.h (SECRET_LIST) and are substituted device-side when
//  typing. They are never echoed in an API reply, logged, or sent over the
//  network — only the reference ${NAME} travels; the value is typed straight to
//  the USB HID. Expansion is applied to text only (not combos).
// ============================================================
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Look up a secret by name for a given user. `user` is the authenticated user's
// name (NULL/"" means none → only global secrets are visible). Lookup is STRICT:
// a name resolves to a secret OWNED by `user`, or to a GLOBAL (unowned) secret of
// that name — never to another user's owned secret. A user-owned secret takes
// precedence over a global one of the same name. Returns value+*out_len, or NULL.
const char *secret_lookup(const char *user, const char *name, int name_len, int *out_len);

// Expand ${NAME} references in `in` (in_len chars) into `out` (capacity out_cap,
// always NUL-terminated), resolving names for `user` per secret_lookup's rules.
//   ${NAME}  -> the secret's value (unknown / not-yours -> nothing emitted)
//   $${      -> literal ${   (escape: a doubled $ collapses to one)
// Output is capped at out_cap-1 chars. Returns the expanded length.
int secret_expand(const char *user, const char *in, int in_len, char *out, int out_cap);

// Number of configured secrets (for status/diagnostics; never exposes values).
int secret_count(void);

#ifdef __cplusplus
}
#endif
