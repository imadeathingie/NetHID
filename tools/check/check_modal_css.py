#!/usr/bin/env python3
"""
check_modal_css.py — every full-screen overlay's card can fit and scroll.

The settings modal grew past the height of a phone screen. It had no max-height
and no overflow, and its overlay centred it with `align-items:center` — so half
the overflow sat ABOVE the top of the screen where no amount of scrolling
reaches it, and the password fields at the bottom were simply unreachable.

Nothing caught it: jsdom does not lay out, so the UI checks cannot measure a
height, and the page has no media queries to inspect. This is a static read of
the four properties whose absence caused it.

    python3 tools/check/check_modal_css.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HTML = ROOT / "NetHID.html"

# selector -> the overlay that contains it. Both halves matter: the card needs
# to clamp and scroll, the overlay needs padding so a tap-outside strip exists
# (that is how these close — Escape only closes the keycode picker).
CARDS = {
    ".settings-card": "#settings-overlay",
    ".lock-card": "#lock-overlay",
    ".km-pbox": ".km-picker",
}

# A card must not be taller than the screen...
CARD_NEEDS = ("max-height", "overflow")
# ...and must not be wider than it either.
CARD_WIDTH = ("max-width", "width:min(")


def rule(css: str, selector: str) -> str:
    """The body of the first rule for `selector`, comments stripped."""
    pat = re.escape(selector) + r"\s*(?:,[^{]*)?\{([^}]*)\}"
    m = re.search(pat, css)
    if not m:
        return ""
    return re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)


def main() -> int:
    css = HTML.read_text()
    bad = False

    for card, overlay in CARDS.items():
        body = rule(css, card)
        if not body:
            print(f"{card}: no such rule — did it get renamed?")
            bad = True
            continue

        flat = body.replace(" ", "")
        for prop in CARD_NEEDS:
            if prop not in flat:
                print(f"{card}: no {prop} — it cannot scroll when it outgrows "
                      f"the screen, and the bottom of it becomes unreachable")
                bad = True
        if not any(w.replace(" ", "") in flat for w in CARD_WIDTH):
            print(f"{card}: no max-width — a fixed width overflows a narrow phone")
            bad = True

        obody = rule(css, overlay).replace(" ", "")
        if not obody:
            print(f"{overlay}: no such rule — did it get renamed?")
            bad = True
        elif "padding:" not in obody:
            print(f"{overlay}: no padding — with the card filling the screen "
                  f"there is no overlay left to tap, and tapping outside is how "
                  f"it closes")
            bad = True

    # A sticky header inside a scrolling card must not leave the card's own top
    # padding above it: content scrolls through that gap. The fix is to move the
    # padding onto the header, so the card's shorthand must not set a top one.
    st = rule(css, ".settings-head").replace(" ", "")
    if "position:sticky" in st:
        # Not the space-stripped copy: the shorthand's sides are separated by
        # the very spaces that strips out.
        card = rule(css, ".settings-card")
        m = re.search(r"padding:\s*([^;]*);", card)
        if m:
            parts = m.group(1).split()
            top = parts[0] if parts else ""
            if top not in ("0", "0px", ""):
                print(f".settings-card: padding-top {top} sits above the sticky "
                      f".settings-head, so scrolling text shows through the gap")
                bad = True

    if bad:
        print("\nA modal that cannot scroll is a dead end on a phone: no media")
        print("queries here to save it, and jsdom cannot measure a layout.")
        return 1

    print(f"modal css OK ({len(CARDS)} overlays checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
