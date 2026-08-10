#!/usr/bin/env python3
"""
test_cond_eval.py — the mini-preprocessor inside dump_hid_descriptor.py.

That checker has now been wrong twice in the same place, and both times the
symptom was a confident verdict about the wrong thing:

  1. It popped the #if stack without ever verifying it was empty at EOF, so an
     unterminated #if — a hard compile error — decoded as "structurally OK".
  2. It treated #elif as a plain #else and never evaluated the condition, so
     two mutually exclusive descriptor branches decoded at once and the result
     was reported as an unbalanced DESCRIPTOR. The bug was in the checker.

A checker whose own logic is untested is a checker reporting on itself.

    python3 tools/check/test_cond_eval.py
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location(
    "dump_hid_descriptor", os.path.join(ROOT, "check", "dump_hid_descriptor.py"))
d = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(d)

fails = 0


def ok(cond, what):
    global fails
    print(("PASS  " if cond else "FAIL  ") + what)
    if not cond:
        fails += 1


def kept(src, defines):
    lines, errors = d.preprocess(src, defines)
    return [l.strip() for l in lines if l.strip()], errors


# ── An #elif chain picks exactly one branch ─────────────────────────────────
CHAIN = """
#if M == 0
A
#elif M == 1
B
#elif M == 2
C
#else
D
#endif
"""
for m, want in [(0, "A"), (1, "B"), (2, "C"), (7, "D")]:
    got, errs = kept(CHAIN, {"M": m})
    ok(got == [want] and not errs, "M=%d selects %s (got %s)" % (m, want, got))

# The third branch is the one the accumulate-the-taken-flag bug would have got
# wrong: after a taken #elif, a later true #elif must still be skipped.
got, _ = kept("""
#if 0
A
#elif V
B
#elif V
C
#endif
""", {"V": 1})
ok(got == ["B"], "a second true #elif does not also run (got %s)" % got)

# ── Comparisons ─────────────────────────────────────────────────────────────
for expr, defs, want in [
    ("M == 2", {"M": 2}, True),   ("M == 2", {"M": 1}, False),
    ("M != 0", {"M": 2}, True),   ("M != 0", {"M": 0}, False),
    ("!A || M != 0", {"A": 0, "M": 0}, True),
    ("!A || M != 0", {"A": 1, "M": 0}, False),
    ("A && M == 1", {"A": 1, "M": 1}, True),
    ("defined(A)", {"A": 0}, True),      # defined, even though it is zero
    ("UNSET == 0", {}, True),            # undefined identifiers are 0
]:
    ok(d.cond_value(expr, defs) is want,
       "#if %-18s with %-20s -> %s" % (expr, defs, want))

# ── Nesting ─────────────────────────────────────────────────────────────────
got, _ = kept("""
#if A
 outer
 #if B
 inner
 #else
 inner-else
 #endif
#endif
""", {"A": 1, "B": 0})
ok(got == ["outer", "inner-else"], "nested #if/#else (got %s)" % got)

got, _ = kept("""
#if A
 outer
 #if B
 inner
 #endif
#endif
""", {"A": 0, "B": 1})
ok(got == [], "a live inner #if inside a dead outer stays dead (got %s)" % got)

# ── Balance ─────────────────────────────────────────────────────────────────
_, errs = kept("#if A\nX\n", {"A": 1})
ok(any("never closed" in e for e in errs),
   "an unterminated #if is an error, not a silent pass")

_, errs = kept("#if A\nX\n#endif\n#endif\n", {"A": 1})
ok(any("no open" in e for e in errs), "a stray #endif is an error")

_, errs = kept("#else\n", {})
ok(any("no open" in e for e in errs), "a stray #else is an error")

_, errs = kept("#elif A\n", {})
ok(any("no open" in e for e in errs), "a stray #elif is an error")

# Unsupported syntax must raise rather than evaluate to something plausible:
# quietly reading `M >> 1` as false would decode the wrong branch and describe
# it as fine.
try:
    d.cond_value("M >> 1", {"M": 4})
    ok(False, "unsupported syntax raises")
except ValueError:
    ok(True, "unsupported syntax raises instead of guessing")

print("\n%d FAILED" % fails if fails else "\nall green")
sys.exit(1 if fails else 0)
