#!/usr/bin/env python3
"""
check_quiet_boot.py — every path that types boot output must honour quiet boot.

Quiet boot suppresses diagnostics typed into the host. It is easy to add a new
message and route it around the gate: that is exactly what happened to
type_to_host(), which carried the "ready" banner, the AP setup address and the
WiFi-failure message and had no check at all, so enabling quiet boot silenced
the per-step lines and left the banner typing into whatever you had focused.

Rules enforced in src/main.cpp:

  1. Every function that calls hid_push_type_string() also calls
     boot_output_allowed().
  2. Nothing calls hid_push_type_string() directly from outside those
     functions — new boot messages go through dbg() or type_to_host().
  3. Nothing outside settings.cpp reads settings()->quiet_boot. That field is
     the STORED value, which is what the settings page renders; anything
     deciding whether to actually type must call
     settings_quiet_boot_effective(), which resolves the loud/quiet boot keys
     on top of it. Reading the stored field would ignore the loud-boot
     override — the one gesture that exists to make a silenced device talk.

Deliberately scoped to main.cpp. /api/text and the macro typer also type, and
must NOT be gated: quiet boot is about unrequested boot noise, not about
refusing work someone asked for.

    python3 tools/check/check_quiet_boot.py
"""

import re
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2] / "src" / "main.cpp"
GATE = "boot_output_allowed"
TYPER = "hid_push_type_string"

# Matches a top-level function definition and captures its body by brace depth.
DEF = re.compile(r'^(?:static\s+)?[\w*]+\s+(\w+)\s*\([^;{]*\)\s*\{', re.M)


def bodies(text):
    for m in DEF.finditer(text):
        i = text.index("{", m.start())
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield m.group(1), text[i:j]


def check_effective(root: Path) -> bool:
    bad = False
    for f in list((root / "src").rglob("*.cpp")) + list((root / "src").rglob("*.c")):
        if f.name == "settings.cpp" or "vendor" in f.parts:
            continue
        body = re.sub(r"//[^\n]*", "", f.read_text(errors="ignore"))
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        if "settings()->quiet_boot" in body:
            print(f"{f.relative_to(root)}: reads the STORED quiet_boot; "
                  f"use settings_quiet_boot_effective()")
            bad = True
    return bad


def main() -> int:
    text = MAIN.read_text()
    # Strip comments so prose mentioning these names is not mistaken for a call.
    stripped = re.sub(r"//[^\n]*", "", text)
    stripped = re.sub(r"/\*.*?\*/", "", stripped, flags=re.S)

    bad = False
    typing_fns = []
    for name, body in bodies(stripped):
        if TYPER + "(" not in body:
            continue
        typing_fns.append(name)
        if GATE + "(" not in body:
            print(f"{name}() types to the host but never calls {GATE}()")
            bad = True

    if not typing_fns:
        print(f"no function in main.cpp calls {TYPER}() - has it been renamed?")
        return 1

    if check_effective(MAIN.parents[1]):
        bad = True

    if bad:
        print("\nRoute new boot messages through dbg() or type_to_host(), or add")
        print(f"an explicit {GATE}() check.")
        return 1

    print(f"quiet boot gates all typed output ({', '.join(sorted(typing_fns))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
