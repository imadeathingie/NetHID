#!/bin/sh
#
# check_build.sh — actually link the firmware, for every board.
#
#     tools/check/check_build.sh            every board
#     tools/check/check_build.sh oledpad    just one
#
# Every other check in this directory is static or host-side. None of them
# compiles the real firmware, so for a long time the tree did not build at all
# while `run-all.sh` reported "all checks passed":
#
#   - an unterminated #if in usb_descriptors.c (a hard compile error)
#   - server.cpp still using the pre-digitizer hid_abs_report_t
#   - oledpad/modular failing on OLED_ENABLE, ENCODERS and a tusb.h include
#
# check_export.sh compiles an exported keymap, which is why it did not notice:
# it never links a firmware image. This does.
#
# Skips cleanly when PICO_SDK_PATH or the ARM toolchain is missing, so it stays
# safe to run on a machine set up only for the host tests.
set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

if [ -z "${PICO_SDK_PATH:-}" ] || [ ! -d "${PICO_SDK_PATH:-/nonexistent}" ]; then
    echo "PICO_SDK_PATH not set or not a directory - skipping (export PICO_SDK_PATH=... to enable)"
    exit 0
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "arm-none-eabi-gcc not on PATH - skipping"
    exit 0
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not on PATH - skipping"
    exit 0
fi

BOARDS=${*:-$(ls keyboards | grep -v '\.md$')}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
JOBS=$( (sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null) || echo 4 )

fails=""
for kb in $BOARDS; do
    [ -f "keyboards/$kb/keyboard.h" ] || continue
    if ! cmake -S "$ROOT" -B "$OUT/$kb" -DPICO_BOARD=pico2_w -DKEYBOARD="$kb" \
              > "$OUT/$kb.cfg.log" 2>&1; then
        echo "  $kb: CONFIGURE FAILED"
        tail -15 "$OUT/$kb.cfg.log" | sed 's/^/    /'
        fails="$fails $kb"
        continue
    fi
    # The default target set includes every per-module firmware on a modular
    # board, which is the half that was broken and the half no check built.
    if cmake --build "$OUT/$kb" -j"$JOBS" > "$OUT/$kb.bld.log" 2>&1; then
        echo "  $kb: ok"
    else
        echo "  $kb: BUILD FAILED"
        grep -iE "error|undefined reference" "$OUT/$kb.bld.log" | head -10 | sed 's/^/    /'
        fails="$fails $kb"
    fi
done

# Every form of the absolute pointer, and the descriptor each one produces.
#
# Two reasons this is not just "the default builds". The interface COUNT differs
# between modes, and it is declared in usb_descriptors.c while the matching
# per-interface state is sized by CFG_TUD_HID in tusb_config.h — disagree and
# the device enumerates and then drops every report on the interface TinyUSB
# does not know about. And modes 0 and 1 are otherwise built by nobody, which is
# exactly how mode 0 came to contain a descriptor that never closed its mouse
# collection.
#
# Mode 2 is the default, so the mystery6x6 image above already IS that build —
# checking its ELF costs nothing, and only the two non-default modes need a
# firmware of their own. Building all three would add a couple of minutes to
# every run to re-link something already on disk.
check_desc() {   # <label> <elf> <expected interfaces>
    if python3 "$ROOT/tools/check/check_usb_descriptor.py" "$2" "$3" > "$OUT/desc.log" 2>&1; then
        echo "  $1: ok, $3 HID interface(s)"
    else
        echo "  $1: BAD DESCRIPTOR"
        sed 's/^/    /' "$OUT/desc.log"
        fails="$fails $1"
    fi
}

if [ -f "$OUT/mystery6x6/nethid.elf" ]; then
    check_desc "(abs mode 2, default)" "$OUT/mystery6x6/nethid.elf" 2
else
    # Says so rather than skipping in silence — reusing another step's output
    # only saves time if its absence is visible when it happens.
    echo "  (abs mode 2, default): no mystery6x6 image in this run - not checked"
fi

for mode in 0 1; do
    if cmake -S "$ROOT" -B "$OUT/_abs$mode" -DPICO_BOARD=pico2_w \
             -DKEYBOARD=mystery6x6 -DABS_MOUSE_MODE="$mode" \
             > "$OUT/_abs$mode.cfg.log" 2>&1 &&
       cmake --build "$OUT/_abs$mode" -j"$JOBS" > "$OUT/_abs$mode.bld.log" 2>&1; then
        check_desc "(abs mode $mode)" "$OUT/_abs$mode/nethid.elf" 1
    else
        echo "  (abs mode $mode): BUILD FAILED"
        grep -iE "error|undefined reference" "$OUT/_abs$mode.bld.log" 2>/dev/null | head -10 | sed 's/^/    /'
        fails="$fails abs-mode-$mode"
    fi
done

# A board with no keyboard at all: the plain network-HID build, whose feature
# guards are the ones a keyboard-only change is most likely to break.
if cmake -S "$ROOT" -B "$OUT/_nokb" -DPICO_BOARD=pico2_w > "$OUT/_nokb.cfg.log" 2>&1 &&
   cmake --build "$OUT/_nokb" -j"$JOBS" > "$OUT/_nokb.bld.log" 2>&1; then
    echo "  (no keyboard): ok"
else
    echo "  (no keyboard): BUILD FAILED"
    grep -iE "error|undefined reference" "$OUT/_nokb.bld.log" 2>/dev/null | head -10 | sed 's/^/    /'
    fails="$fails no-keyboard"
fi

# No web server. This is the branch that decides whether every "compiled out"
# stub actually exists — a store whose real body is guarded but whose stubs are
# never compiled links fine everywhere else and fails only here. The password
# store's stub branch had a bare NULL with no header to declare it, which no
# other configuration would ever have caught.
if cmake -S "$ROOT" -B "$OUT/_noweb" -DPICO_BOARD=pico2_w -DKEYBOARD=mystery6x6 \
        -DENABLE_WEB=OFF -DENABLE_HTTPS=OFF > "$OUT/_noweb.cfg.log" 2>&1 &&
   cmake --build "$OUT/_noweb" -j"$JOBS" > "$OUT/_noweb.bld.log" 2>&1; then
    echo "  (no web server): ok"
else
    echo "  (no web server): BUILD FAILED"
    grep -iE "error|undefined reference" "$OUT/_noweb.bld.log" 2>/dev/null | head -10 | sed 's/^/    /'
    fails="$fails no-web"
fi

if [ -n "$fails" ]; then
    echo "FAILED:$fails"
    exit 1
fi
echo "all boards build"
