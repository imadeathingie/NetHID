// ============================================================
//  secrets.cpp — secret table (from env.h SECRET_LIST) + ${NAME} expansion.
// ============================================================
#include "secrets.h"
#include "config.h"     // pulls in env.h (SECRET_LIST) + defaults
#include <string.h>

typedef struct { const char *owner; const char *name; const char *value; } nethid_secret_t;

// Expand the X-macro list into an array. SECRET(name,value) is a GLOBAL secret
// (owner == NULL, visible to everyone); SECRET_FOR(user,name,value) is owned by
// that user (visible only to them). A trailing sentinel keeps the array
// non-empty even when SECRET_LIST is empty (zero-length arrays are invalid C++)
// and is excluded from the count.
#define SECRET(n, v)        { NULL, (n), (v) },
#define SECRET_FOR(u, n, v) { (u),  (n), (v) },
static const nethid_secret_t SECRETS[] = {
    SECRET_LIST
    { NULL, NULL, NULL }   // sentinel
};
#undef SECRET
#undef SECRET_FOR

static const int SECRET_N = (int)(sizeof(SECRETS) / sizeof(SECRETS[0])) - 1;

int secret_count(void) { return SECRET_N; }

const char *secret_lookup(const char *user, const char *name, int name_len, int *out_len) {
    // Strict resolution: a secret owned by `user` wins; otherwise a global
    // (unowned) secret of that name; never another user's owned secret.
    const char *global_val = NULL;
    int         global_len = 0;
    for (int i = 0; i < SECRET_N; ++i) {
        if ((int)strlen(SECRETS[i].name) != name_len) continue;
        if (memcmp(SECRETS[i].name, name, (size_t)name_len) != 0) continue;

        if (SECRETS[i].owner == NULL) {                 // global candidate
            if (!global_val) {
                global_val = SECRETS[i].value;
                global_len = (int)strlen(SECRETS[i].value);
            }
            continue;
        }
        // Owned secret: only visible to its owner.
        if (user && user[0] && strcmp(SECRETS[i].owner, user) == 0) {
            if (out_len) *out_len = (int)strlen(SECRETS[i].value);
            return SECRETS[i].value;                    // user-owned takes precedence
        }
    }
    if (global_val) { if (out_len) *out_len = global_len; return global_val; }
    if (out_len) *out_len = 0;
    return NULL;
}

int secret_expand(const char *user, const char *in, int in_len, char *out, int out_cap) {
    int o = 0;
    int i = 0;
    while (i < in_len && o < out_cap - 1) {
        char c = in[i];

        // "$$" -> literal "$"
        if (c == '$' && i + 1 < in_len && in[i+1] == '$') {
            out[o++] = '$';
            i += 2;
            continue;
        }

        // "${NAME}" -> secret value (unknown / not yours -> nothing)
        if (c == '$' && i + 1 < in_len && in[i+1] == '{') {
            int j = i + 2;
            while (j < in_len && in[j] != '}') ++j;
            if (j < in_len) {                       // found closing brace
                int vlen = 0;
                const char *val = secret_lookup(user, in + i + 2, j - (i + 2), &vlen);
                if (val) {
                    for (int k = 0; k < vlen && o < out_cap - 1; ++k)
                        out[o++] = val[k];
                }
                i = j + 1;                          // skip past '}'
                continue;
            }
            // no closing brace before end -> fall through, emit '$' literally
        }

        out[o++] = c;
        ++i;
    }
    out[o] = '\0';
    return o;
}
