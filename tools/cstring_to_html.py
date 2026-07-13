#!/usr/bin/env python3
"""
cstring_to_html.py — extract a C string literal from web.cpp back to HTML.

WHY THIS EXISTS
---------------
The web UI lives inside web.cpp as C string literals (MAIN_HTML / LOGIN_HTML).
To edit the UI you want it back as a normal .html file you can open in a
browser. This script pulls a named literal out of a .cpp file and decodes the
C escapes (\\n, \\", \\\\, \\t) back into plain text.

Round-trips with html_to_cstring.py:
    cstring_to_html.py web.cpp -n MAIN_HTML -o NetHID.html   # extract & edit
    # ...edit NetHID.html in a browser...
    html_to_cstring.py NetHID.html -n MAIN_HTML -o main.txt  # regenerate
    # ...paste main.txt back into web.cpp...

USAGE
-----
  python3 tools/cstring_to_html.py src/web.cpp                  # MAIN_HTML -> stdout
  python3 tools/cstring_to_html.py src/web.cpp -n LOGIN_HTML
  python3 tools/cstring_to_html.py src/web.cpp -o NetHID.html
"""

import argparse
import re
import sys


def extract_literal(cpp_text: str, var_name: str) -> str:
    """Find `static const char VAR[] = "..." "..." ... ;` and return the
    decoded string contents."""
    # Locate the declaration line.
    decl = re.search(rf'\b{re.escape(var_name)}\s*\[\s*\]\s*=', cpp_text)
    if not decl:
        raise ValueError(f"could not find a literal named {var_name!r}")

    # From the '=' scan forward, collecting the contents of double-quoted
    # string fragments until we hit the terminating semicolon that is OUTSIDE
    # any string. This correctly skips ';' characters that appear inside the
    # HTML/CSS/JS (of which there are many).
    i = decl.end()
    n = len(cpp_text)
    parts = []
    in_str = False
    buf = []
    while i < n:
        c = cpp_text[i]
        if in_str:
            if c == '\\' and i + 1 < n:
                buf.append(cpp_text[i:i + 2])  # keep escape pair intact
                i += 2
                continue
            if c == '"':
                in_str = False
                parts.append(''.join(buf))
                buf = []
                i += 1
                continue
            buf.append(c)
            i += 1
            continue
        else:
            if c == '"':
                in_str = True
                i += 1
                continue
            if c == ';':
                break  # end of the declaration
            # whitespace / comments / concatenation between fragments
            i += 1
            continue

    escaped = ''.join(parts)
    return c_unescape(escaped)


def c_unescape(s: str) -> str:
    """Decode the subset of C escapes used in these literals."""
    out = []
    i = 0
    n = len(s)
    simple = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\',
              "'": "'", '0': '\0', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}
    while i < n:
        c = s[i]
        if c == '\\' and i + 1 < n:
            nxt = s[i + 1]
            if nxt in simple:
                out.append(simple[nxt])
                i += 2
                continue
            if nxt == 'x':  # \xHH
                hexd = s[i + 2:i + 4]
                try:
                    out.append(chr(int(hexd, 16)))
                    i += 4
                    continue
                except ValueError:
                    pass
            # Unknown escape: keep the escaped char literally.
            out.append(nxt)
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract a C string literal to HTML.")
    ap.add_argument("cpp", help="Path to the .cpp file (e.g. src/web.cpp)")
    ap.add_argument("-n", "--name", default="MAIN_HTML",
                    help="C variable name to extract (default: MAIN_HTML)")
    ap.add_argument("-o", "--out", help="Write to this file instead of stdout")
    args = ap.parse_args()

    try:
        with open(args.cpp, "r", encoding="utf-8") as f:
            cpp = f.read()
    except OSError as e:
        print(f"error: cannot read {args.cpp}: {e}", file=sys.stderr)
        return 1

    try:
        html = extract_literal(cpp, args.name)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(html)
        print(f"wrote {args.out} ({len(html)} bytes)", file=sys.stderr)
    else:
        sys.stdout.write(html)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
