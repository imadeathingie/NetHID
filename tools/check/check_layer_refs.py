#!/usr/bin/env python3
"""
check_layer_refs.py — every layer a keycode switches to survives the export.

    python3 tools/check/check_layer_refs.py keymap.json keymap.cpp

Dropping trailing all-transparent layers is right, but dropping one that MO(3)
points at is not a smaller keymap — it is a broken one. kb_layer_lookup() skips
layers past keymap_layer_count, so the key silently does nothing and the file
still compiles.
"""
import importlib.util
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
ARGS = sys.argv[1:]

spec = importlib.util.spec_from_file_location(
    "j", os.path.join(ROOT, "tools", "keyboard", "json_to_keymap.py"))
j = importlib.util.module_from_spec(spec)
# json_to_keymap parses argv at import; give it something harmless and put the
# real arguments back afterwards. Reading sys.argv after this without saving it
# first is how this check silently examined /dev/null instead of the keymap.
sys.argv = ["x", "/dev/null"]
try:
    spec.loader.exec_module(j)
except SystemExit:
    pass

if len(ARGS) != 2:
    sys.exit(__doc__.strip())

d = json.load(open(ARGS[0], encoding="utf-8"))
src = open(ARGS[1], encoding="utf-8").read()

# Only the keymaps[] block. encoder_map entries have the same `[n] = {` shape,
# and counting those too inflates the total — which makes this check pass when
# it should fail, the worst direction for a check to be wrong in.
m = re.search(r"keymaps\[\]\[MATRIX_ROWS\]\[MATRIX_COLS\]\s*=\s*\{(.*?)^\};",
              src, re.S | re.M)
if not m:
    sys.exit("  could not find the keymaps[] block in the generated file")
emitted = len(re.findall(r"^\s*\[\d+\] = (?:LAYOUT\(|\{)", m.group(1), re.M))
need = j.highest_referenced_layer(d) + 1

if emitted < max(1, need):
    print("  referenced layers survived: NO (%d emitted, %d needed)" % (emitted, need))
    sys.exit(1)
print("  %d layer(s) emitted, highest referenced is %d" % (emitted, need - 1))
