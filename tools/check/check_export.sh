#!/bin/sh
#
# check_export.sh — the exported JSON converts to a keymap.cpp that compiles.
#
# Exercises the real path end to end: the web editor's Export button under jsdom,
# then tools/keyboard/json_to_keymap.py over what it produced, then a compile against the
# real headers. A generated file that names the wrong key still compiles, so the
# compile is necessary but not sufficient — hence also checking that macros and
# encoders survive, and that the board's LAYOUT() was used where it exists.
#
# Needs jsdom; skips cleanly without it.
set -e
cd "$(dirname "$0")/../.."
BOARD=${1:-oledpad}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if ! node -e "require('jsdom')" 2>/dev/null; then
    echo "jsdom not installed - skipping (npm i jsdom to enable)"
    exit 0
fi

python3 tools/web/preview.py >/dev/null
node tools/web/export_capture.js "$BOARD" "$TMP" >/dev/null
python3 tools/keyboard/json_to_keymap.py "$TMP/keymap.json" -o "$TMP/keymap.cpp"

g++ -std=c++17 -fsyntax-only -Wall -Wextra \
    -Itools/kbtest/stubs -Iinclude -Isrc "-Ikeyboards/$BOARD" \
    -DENABLE_KEYBOARD=1 -DKB_FEATURE_LAYERS=1 -DKB_FEATURE_TAPPING=1 \
    -DKB_FEATURE_MACROS=1 -DKB_FEATURE_MOUSEKEYS=1 -DKB_FEATURE_CONSUMER=1 \
    -DKB_FEATURE_DYNAMIC_KEYMAP=1 -x c++ "$TMP/keymap.cpp"
echo "$BOARD: generated keymap.cpp compiles"

# Layers referenced by a keycode must survive the trim, or MO(n) silently does
# nothing.
python3 tools/check/check_layer_refs.py "$TMP/keymap.json" "$TMP/keymap.cpp"

grep -q "LAYOUT("      "$TMP/keymap.cpp" && echo "$BOARD: written through LAYOUT()"
grep -q "kb_macro_user" "$TMP/keymap.cpp" && echo "$BOARD: macros included"
grep -q "encoder_map"   "$TMP/keymap.cpp" && echo "$BOARD: encoders included"
exit 0
