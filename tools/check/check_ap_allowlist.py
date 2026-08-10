#!/usr/bin/env python3
"""
check_ap_allowlist.py — verify AP setup mode's API allowlist against the routes
that actually exist in web.cpp.

Two directions, and the second is the one that actually bites.

FORWARD  every allowlisted path must be a real route. A typo fails silently:
         the path never matches, the endpoint 403s, and setup mode becomes a
         page that cannot save anything.

REVERSE  every path the UI needs in setup mode must be allowlisted. This is how
         /api/challenge got missed — login is a three-step handshake and only
         two steps were listed, so the login page reported "Cannot start login"
         and there was no way into setup mode at all.

The reverse check reads the login page and the tab fragments that setup mode
serves, and requires every endpoint they call to be allowed. Control endpoints
the page shell also references (/api/key, /api/text, /api/wake, IR/RF) are
correctly blocked and are not part of that set.

    python3 tools/check/check_ap_allowlist.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "src" / "web.cpp"
MAIN_HTML = ROOT / "NetHID.html"
LOGIN_HTML = ROOT / "NetHID_login.html"

# Tabs setup mode serves. Keep in step with the tab filter in web.cpp.
AP_TABS = ("wifi", "keymap", "settings")

# Called by the page shell on every load, so they must work before any tab does.
SHELL_CRITICAL = ("/api/whoami", "/api/ping")

CALLS = re.compile(r"""(?:fetch|api)\(\s*['"](/[^'"?]+)""")
TAB = re.compile(r"<!--@@TAB (\w+)@@-->(.*?)<!--@@ENDTAB@@-->", re.S)
# Served by the catch-all page handler rather than a strcmp(path, ...) route.
NOT_ROUTES = {"/", "/favicon.ico"}


def main() -> int:
    s = WEB.read_text()
    if "static const char *allowed[]" not in s:
        print("no AP allowlist found — is ENABLE_AP_MODE code still present?")
        return 1

    blk = s[s.index("static const char *allowed[]"):s.index("bool ok = false;")]
    # Strip // comments first: the rationale next to these entries quotes error
    # messages, and a naive literal scan reads those as paths.
    blk = re.sub(r'//[^\n]*', '', blk)
    head, _, tail = blk.partition("allowed_prefix")
    # Only quoted strings that look like paths.
    exact = [p for p in re.findall(r'"([^"]+)"', head) if p.startswith("/")]
    prefix = [p for p in re.findall(r'"([^"]+)"', tail) if p.startswith("/")]

    routes = set(re.findall(r'strcmp\(\s*path\s*,\s*"([^"]+)"\s*\)', s))
    routes |= set(re.findall(r'strncmp\(\s*path\s*,\s*"([^"]+)"', s))

    allowed = set(exact)

    # ── Reverse: what the UI needs ──────────────────────────────────────────
    needed = {}
    if LOGIN_HTML.exists():
        for u in CALLS.findall(LOGIN_HTML.read_text(errors="ignore")):
            needed.setdefault(u, "login page")
    if MAIN_HTML.exists():
        html = MAIN_HTML.read_text(errors="ignore")
        for name, block in TAB.findall(html):
            if name in AP_TABS:
                for u in CALLS.findall(block):
                    needed.setdefault(u, f"{name} tab")
    for u in SHELL_CRITICAL:
        needed.setdefault(u, "page shell")

    bad = False
    for u, who in sorted(needed.items()):
        if u in allowed:
            continue
        if any(u.startswith(p) for p in prefix):
            continue
        print(f"needed by {who} but NOT allowlisted: {u}")
        bad = True

    for p in exact:
        if p not in routes and p not in NOT_ROUTES:
            print(f"allowlisted but no such route: {p}")
            bad = True
    for p in prefix:
        if not any(r.startswith(p) for r in routes):
            print(f"allowlisted prefix matches nothing: {p}")
            bad = True

    if bad:
        print("\nIn setup mode an unlisted path is a 403 and a listed-but-missing")
        print("one is dead weight. Both leave you unable to configure the device.")
        return 1

    print(f"AP allowlist OK ({len(exact)} paths, {len(prefix)} prefix; "
          f"{len(needed)} UI calls checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
