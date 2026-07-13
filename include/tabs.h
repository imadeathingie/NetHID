#pragma once
// ============================================================
//  tabs.h — per-user web-UI tab access.
//
//  The web UI is served per-user: web.cpp sends only the tab fragments a user
//  is allowed to see. ADMIN_USER sees every tab. Grants for other users come
//  from env.h (TAB_GRANTS, a list of TAB_FOR(user, id)). If no grants are
//  configured at all, the UI is ungated (everyone sees all tabs).
// ============================================================
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// May `user` be served the tab `id`?  Rules:
//   - ADMIN_USER  -> always true (sees everything)
//   - no grants configured anywhere -> true (ungated, back-compat)
//   - TAB_FOR("*", id)      -> true for everyone (public tab)
//   - TAB_FOR(user, id)     -> true for that user
//   - otherwise             -> false
bool tab_user_allowed(const char *user, const char *id);

// Number of configured grants (0 = ungated).
int  tab_grant_count(void);

#ifdef __cplusplus
}
#endif
