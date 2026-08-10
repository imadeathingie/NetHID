#!/bin/sh
# Regenerate the preview and check it runs. Skips cleanly without jsdom rather
# than failing a build for a missing dev dependency.
set -e
cd "$(dirname "$0")/../.."
python3 tools/web/preview.py >/dev/null
if ! node -e "require('jsdom')" 2>/dev/null; then
    echo "jsdom not installed - built preview.html but skipped the check"
    exit 0
fi
exec node tools/check/check_preview.js preview.html
