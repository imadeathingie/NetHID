#!/bin/sh
# Wrapper so the jsdom-based password-panel check skips cleanly when jsdom is
# absent, rather than failing a build for a missing dev dependency.
set -e
cd "$(dirname "$0")/../.."
if ! node -e "require('jsdom')" 2>/dev/null; then
    echo "jsdom not installed - skipping (npm i jsdom@24 to enable)"
    exit 0
fi
exec node tools/check/check_password_ui.js NetHID.html
