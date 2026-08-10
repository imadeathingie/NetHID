#!/usr/bin/env python3
"""
dump_hid_descriptor.py — decode the HID report descriptor from the source.

    python3 tools/check/dump_hid_descriptor.py

Prints the item stream with collection nesting and checks it is balanced. An
unbalanced or malformed descriptor is not rejected by the device — the HOST
parses it, and a host that cannot make sense of a collection typically ignores
that collection in silence. So the failure looks like "my absolute mouse does
nothing" rather than like a descriptor problem, which is why this exists.

Verifies structure only. It cannot tell you whether a particular host will
choose to act on a given usage; that needs the hardware.
"""
import itertools
import os
import re
import sys

# The configuration the firmware ships with. Override with -DNAME=0 to decode
# the other branch, or pass --all to check every combination.
DEFAULTS = {"ENABLE_ABS_MOUSE": 1, "ABS_MOUSE_MODE": 2}

# The defines that select between descriptor branches, and every value each can
# take. --all sweeps the product, so a branch nobody builds by default still
# gets checked — the unterminated #if that stopped the firmware compiling lived
# in exactly such a branch, and the default run could not see it.
SWEEP = {"ENABLE_ABS_MOUSE": (0, 1), "ABS_MOUSE_MODE": (0, 1, 2)}

# Every descriptor array in the file. ABS_MOUSE_MODE 2 puts the absolute pointer
# in a second one, on its own interface; decoding only the first would call that
# build "structurally OK" without having looked at half of it.
ARRAYS = ["desc_hid_report[]", "desc_hid_report_abs[]"]

# Order matters: == and != must be tried before a bare !.
_COND_TOKEN = re.compile(r"[A-Za-z_]\w*|\d+|==|!=|>=|<=|\|\||&&|[!<>()]")


def cond_value(expr, defines):
    """Evaluate a #if expression against `defines`.

    Only what this descriptor actually uses is supported: defined(), !, &&, ||,
    the comparisons == != < > <= >=, and parentheses over identifiers and
    integers. An undefined identifier is 0, as in the C preprocessor. Anything
    else raises rather than guessing — silently mis-reading a conditional would
    decode the wrong branch and the output would look perfectly fine, which is
    the failure this whole file exists to prevent.
    """
    expr = re.sub(r"\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)",
                  lambda m: str(int((m.group(1) or m.group(2)) in defines)),
                  expr)

    out, pos = [], 0
    for m in _COND_TOKEN.finditer(expr):
        if expr[pos:m.start()].strip():
            raise ValueError("unsupported syntax in #if: %r" % expr)
        pos = m.end()
        t = m.group(0)
        if t == "!":
            out.append(" not ")
        elif t == "&&":
            out.append(" and ")
        elif t == "||":
            out.append(" or ")
        elif t in ("==", "!=", "<", ">", "<=", ">="):
            out.append(" %s " % t)
        elif t in "()" or t.isdigit():
            out.append(t)
        else:
            out.append(str(int(defines.get(t, 0))))
    if expr[pos:].strip():
        raise ValueError("unsupported syntax in #if: %r" % expr)
    try:
        return bool(eval("".join(out), {"__builtins__": {}}, {}))
    except Exception as e:
        # e.g. an unsupported operator that tokenised into adjacent terms.
        # Reported as an error, never quietly treated as false.
        raise ValueError("cannot evaluate #if %r (%s)" % (expr, e))

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src", "usb_descriptors.c")

TAGS = {
    0x04: "Usage Page", 0x08: "Usage", 0x14: "Logical Min", 0x24: "Logical Max",
    0x74: "Report Size", 0x94: "Report Count", 0x80: "Input", 0x84: "Report ID",
    0xA0: "Collection", 0xC0: "End Collection", 0x18: "Usage Min",
    0x28: "Usage Max", 0x90: "Output", 0xB0: "Feature",
    0x34: "Physical Min", 0x44: "Physical Max", 0x54: "Unit Exponent",
    0x64: "Unit", 0xA4: "Push", 0xB4: "Pop",
}
COLL = {0x00: "Physical", 0x01: "Application", 0x02: "Logical"}


def preprocess(raw, defines):
    """Return (kept_lines, errors) for `raw` under `defines`.

    Honours #if/#else/#endif. Without this both branches of a conditional are
    decoded and the byte counts are nonsense — which is exactly how a
    "structurally OK" report came out 11 bytes long.

    Balance is verified, not assumed. Popping a stack without ever checking it
    is empty at the end let an unterminated #if — a hard compile error — reach
    a "structurally OK" verdict, which is worse than having no check at all.
    """
    kept, errors = [], []
    stack = [True]        # parallel to the #if nesting; [0] is the file itself
    taken = [True]        # has any branch of this chain been taken yet?
    opened = []           # line numbers, so an unterminated #if can be located

    def push(live):
        stack.append(stack[-1] and live)
        taken.append(live)

    for n, line in enumerate(raw.split("\n"), 1):
        t = line.strip()

        m = re.match(r"#\s*if(n?)def\s+(\w+)", t)
        if m:
            push((m.group(2) in defines) != bool(m.group(1)))
            opened.append(n)
            continue
        m = re.match(r"#\s*if\b(.*)", t)
        if m:
            try:
                live = cond_value(m.group(1), defines)
            except ValueError as e:
                errors.append("line %d: %s" % (n, e))
                live = False
            push(live)
            opened.append(n)
            continue
        # #elif must EVALUATE its condition. Treating it as a plain #else — which
        # this did — makes the branch after a false #if unconditionally live, so
        # a chain of mutually exclusive modes decodes as two of them at once and
        # reports the result as an unbalanced descriptor. The bug was in here,
        # not in the descriptor.
        m = re.match(r"#\s*elif\b(.*)", t)
        if m:
            if len(stack) == 1:
                errors.append("line %d: #elif with no open #if" % n)
                continue
            try:
                live = cond_value(m.group(1), defines)
            except ValueError as e:
                errors.append("line %d: %s" % (n, e))
                live = False
            was = taken[-1]              # has any earlier branch already run?
            live = live and not was
            stack.pop(); taken.pop()
            push(live)
            # push() sets taken[-1] to this branch alone. The chain needs the
            # ACCUMULATED answer, or a third #elif whose condition is true would
            # run after a second one already had.
            taken[-1] = was or live
            continue
        if re.match(r"#\s*else\b", t):
            if len(stack) == 1:
                errors.append("line %d: #else with no open #if" % n)
                continue
            was = taken.pop()
            stack.pop()
            push(not was)
            continue
        if re.match(r"#\s*endif\b", t):
            if len(stack) == 1:
                errors.append("line %d: #endif with no open #if" % n)
                continue
            stack.pop()
            taken.pop()
            opened.pop()
            continue
        if all(stack):
            kept.append(line)

    for n in opened:
        errors.append("line %d: #if never closed (this will not compile)" % n)
    return kept, errors


def decode(raw, defines, verbose=True):
    # Errors always print; the item stream is what --all suppresses.
    say = print if verbose else (lambda *a, **k: None)

    kept, errors = preprocess(raw, defines)
    for e in errors:
        print("  preprocessor: %s" % e)
    bad = len(errors)
    body = re.sub(r"//[^\n]*", "", "\n".join(kept))
    # Block comments too: the shared ABS_POINTER_BODY macro annotates its items
    # with /* */ because it is backslash-continued, and a // comment would eat
    # the continuation.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    data = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]

    depth, k = 0, 0
    bits = {}
    rid = None
    size = count = 0
    # Global item state. Physical Min/Max are GLOBAL — they stay in effect for
    # every main item that follows until changed, which is not obvious from
    # reading the descriptor top to bottom and cost this project a Windows bug:
    # Physical 0..32767 declared for absolute X/Y was still in force for the
    # Wheel below it (logical -127..127), so a host converting to physical units
    # read a wheel of 0 as the middle of 0..32767 and scrolled on every report.
    lmin = lmax = pmin = pmax = 0
    while k < len(data):
        b = data[k]
        n = b & 3
        n = 4 if n == 3 else n
        tag = b & 0xFC
        arg = data[k + 1:k + 1 + n]
        val = sum(v << (8 * i) for i, v in enumerate(arg))
        name = TAGS.get(tag, "0x%02X" % tag)

        if tag == 0xC0:
            depth -= 1
            if depth < 0:
                print("  unbalanced: End Collection with nothing open")
                bad += 1
        pad = "  " * max(depth, 0)

        if tag == 0xA0:
            say("%s%s (%s)" % (pad, name, COLL.get(val, "0x%02X" % val)))
            depth += 1
        elif tag == 0xC0:
            say("%s%s" % (pad, name))
        elif tag == 0x84:
            rid = val
            say("%sReport ID %d" % (pad, rid))
        else:
            if tag == 0x74:
                size = val
            if tag == 0x94:
                count = val
            if tag == 0x14:
                lmin = val
            if tag == 0x24:
                lmax = val
            if tag == 0x34:
                pmin = val
            if tag == 0x44:
                pmax = val
            if tag == 0x80:
                if rid is not None:
                    bits[rid] = bits.get(rid, 0) + size * count
                # Both zero means "physical units are the logical units", which
                # is the only thing this device ever wants. Anything else is
                # either a deliberate real-world unit (this descriptor declares
                # none) or a stale global that has leaked onto an item it was
                # never meant to describe.
                if (pmin, pmax) != (0, 0) and (pmin, pmax) != (lmin, lmax):
                    print("  physical range %d..%d is in effect for an Input "
                          "whose logical range is %d..%d - Physical Min/Max are "
                          "GLOBAL and leak forward; a host that converts to "
                          "physical units will rescale this field"
                          % (pmin, pmax, lmin, lmax))
                    bad += 1
            say("%s%s = %d" % (pad, name, val))
        k += 1 + n

    say("")
    for r in sorted(bits):
        say("report %d: %d bits = %d bytes payload (+1 for the report ID)"
            % (r, bits[r], (bits[r] + 7) // 8))
    if depth != 0:
        print("  UNBALANCED: %d collection(s) left open" % depth)
        bad += 1
    return bad


def macro_body(src, name):
    """The body of a backslash-continued object-like macro, or ''."""
    m = re.search(r"#\s*define\s+%s\b(.*?)(?<!\\)\n" % re.escape(name), src, re.S)
    if not m:
        return ""
    return m.group(1).replace("\\\n", "\n")


def arrays(src):
    """Each descriptor array's source text, with shared macros expanded.

    The absolute-pointer items live in a macro so modes 0 and 2 cannot drift
    apart. That means the array's own text does not contain them, and a decoder
    that reads the file literally would silently check a descriptor missing its
    entire pointer — passing, while describing nothing.
    """
    body = macro_body(src, "ABS_POINTER_BODY")
    out = []
    for name in ARRAYS:
        try:
            i = src.index(name)
        except ValueError:
            continue                      # not present in this build's source
        j = src.index("};", i)
        out.append((name, src[i:j].replace("ABS_POINTER_BODY", body)))
    return out


def decode_all(src, defines, verbose=True):
    bad = 0
    for name, raw in arrays(src):
        if verbose and len(ARRAYS) > 1:
            print("── %s" % name)
        bad += decode(raw, defines, verbose)
    return bad


def main() -> int:
    src = open(SRC, encoding="utf-8").read()

    args = sys.argv[1:]
    defines = dict(DEFAULTS)
    for a in args:
        if a.startswith("-D"):
            k, _, v = a[2:].partition("=")
            defines[k] = int(v or 1)

    if "--all" in args:
        # Every combination, because a descriptor branch that is never decoded
        # is a branch nobody has checked. Item streams are suppressed; six
        # full dumps is noise nobody reads, and the verdict is the point.
        bad = 0
        keys = list(SWEEP)
        for combo in itertools.product(*(SWEEP[k] for k in keys)):
            d = dict(defines, **dict(zip(keys, combo)))
            label = " ".join("%s=%d" % (k, d[k]) for k in keys)
            print("── %s" % label)
            n = decode_all(src, d, verbose=False)
            print("   %s\n" % ("MALFORMED" if n else "structurally OK"))
            bad += n
        print("%s" % ("MALFORMED" if bad else "all variants structurally OK"))
        return 1 if bad else 0

    bad = decode_all(src, defines)
    print("\n%s" % ("MALFORMED" if bad else "structurally OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
