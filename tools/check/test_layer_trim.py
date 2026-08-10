#!/usr/bin/env python3
"""
test_layer_trim.py — which layers survive the C export.

Dropping trailing all-transparent layers is easy to get subtly wrong, and every
way of getting it wrong produces a file that compiles:

  - drop one a key REFERS to  -> MO(3) silently does nothing
  - drop a middle one         -> every layer above shifts down
  - ignore encoders           -> a layer that only remaps a knob disappears
  - drop layer 0              -> does not compile, at least that one is loud

    python3 tools/check/test_layer_trim.py
"""
import os
import sys
import importlib.util

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location(
    "j", os.path.join(ROOT, "tools", "keyboard", "json_to_keymap.py"))
j = importlib.util.module_from_spec(spec)
sys.argv = ["x", "/dev/null"]
try:
    spec.loader.exec_module(j)
except SystemExit:
    pass

TRNS, A, NO = 1, 0x04, 0
MO = lambda n: 0x5100 | n
LT = lambda l, k: 0x4000 | (l << 8) | k
VOLU = 0x5A01

fails = 0


def ck(name, got, want):
    global fails
    ok = got == want
    print("%-52s %s" % (name, "PASS" if ok else "FAIL  got %r want %r" % (got, want)))
    if not ok:
        fails += 1


def doc(layers, enc=None, cols=2):
    return {"rows": 1, "cols": cols, "layers": layers, "encoder_map": enc}


full = [A, A]
empty = [TRNS, TRNS]

ck("trailing empties are dropped",
   j.used_layer_count(doc([full, empty, empty, empty])), 1)

ck("layer 0 is kept even if empty",
   j.used_layer_count(doc([empty, empty])), 1)

ck("an empty layer below a populated one is kept",
   j.used_layer_count(doc([full, empty, full, empty])), 3)

ck("a layer referenced by MO() is kept though empty",
   j.used_layer_count(doc([[MO(2), A], empty, empty, empty])), 3)

ck("a layer referenced by LT() is kept though empty",
   j.used_layer_count(doc([[LT(3, A), A], empty, empty, empty, empty])), 4)

ck("a reference from an encoder counts too",
   j.used_layer_count(doc([full, empty, empty],
                          enc=[[[MO(2), A, A]], [[TRNS]*3], [[TRNS]*3]])), 3)

ck("a layer that only remaps an encoder survives",
   j.used_layer_count(doc([full, empty, empty],
                          enc=[[[A, A, A]], [[VOLU, TRNS, TRNS]], [[TRNS]*3]])), 2)

ck("nothing is dropped when every layer has content",
   j.used_layer_count(doc([full, full, full])), 3)

ck("KC_NO is not transparent",
   j.used_layer_count(doc([full, [NO, NO]])), 2)

print("\n%s" % ("FAILURES" if fails else "all green"))
sys.exit(1 if fails else 0)
