// ============================================================
//  tabs.cpp — per-user tab grants (from env.h TAB_GRANTS).
// ============================================================
#include "tabs.h"
#include "config.h"     // pulls in env.h (TAB_GRANTS, ADMIN_USER) + defaults
#include <string.h>

typedef struct { const char *user; const char *id; } nethid_tab_grant_t;

#define TAB_FOR(u, t) { (u), (t) },
static const nethid_tab_grant_t TAB_GRANTS_ARR[] = {
    TAB_GRANTS
    { NULL, NULL }   // sentinel keeps the array non-empty; excluded from count
};
#undef TAB_FOR

static const int TAB_GRANT_N = (int)(sizeof(TAB_GRANTS_ARR) / sizeof(TAB_GRANTS_ARR[0])) - 1;

int tab_grant_count(void) { return TAB_GRANT_N; }

bool tab_user_allowed(const char *user, const char *id) {
    // Admin sees every tab.
    if (user && user[0] && strcmp(user, ADMIN_USER) == 0) return true;
    // No grants configured at all -> ungated (everyone sees all).
    if (TAB_GRANT_N == 0) return true;
    for (int i = 0; i < TAB_GRANT_N; ++i) {
        if (strcmp(TAB_GRANTS_ARR[i].id, id) != 0) continue;
        const char *owner = TAB_GRANTS_ARR[i].user;
        if (strcmp(owner, "*") == 0) return true;                 // public tab
        if (user && user[0] && strcmp(owner, user) == 0) return true;
    }
    return false;
}
