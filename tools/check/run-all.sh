#!/bin/sh
#
# run-all.sh — every static check, plus the host test suite.
#
#     tools/check/run-all.sh          everything
#     tools/check/run-all.sh --fast   skip the jsdom and compile-heavy ones
#
# Checks that need node+jsdom skip themselves cleanly when it is absent, so this
# is safe to run anywhere. Exits non-zero if anything failed, and says which.
set -u
cd "$(dirname "$0")/../.."

FAST=0
[ "${1:-}" = "--fast" ] && FAST=1

fails=""
run() {
    name=$1; shift
    printf '%-24s ' "$name"
    if out=$("$@" 2>&1); then
        echo "$out" | grep -Ev "^(g\+\+|make|\s)" | tail -1
    else
        echo "FAILED"
        echo "$out" | sed 's/^/    /'
        fails="$fails $name"
    fi
}

echo "── source ─────────────────────────────────────────────"
run includes     python3 tools/check/check_includes.py
run typed-ascii  python3 tools/check/check_typed_ascii.py
run quiet-boot   python3 tools/check/check_quiet_boot.py
run ap-allowlist python3 tools/check/check_ap_allowlist.py
run pins         python3 tools/check/check_pins.py
run keycodes     python3 tools/check/check_keycode_tables.py
run layouts      python3 tools/check/check_layout_tables.py
run modal-css    python3 tools/check/check_modal_css.py
run layer-trim   python3 tools/check/test_layer_trim.py
run cond-eval    python3 tools/check/test_cond_eval.py
run hid-descr    python3 tools/check/dump_hid_descriptor.py --all

echo
echo "── host tests ─────────────────────────────────────────"
# NOT piped to tail: a pipeline's exit status is the last command's, so
# `make check | tail -1` always succeeds and a failing suite reports as passing.
# That masked a real build break once.
run kbtest       sh -c 'cd tools/kbtest && make check 2>&1'

if [ "$FAST" = "1" ]; then
    echo
    echo "(--fast: skipped the jsdom and export checks)"
else
    echo
    echo "── web ui ─────────────────────────────────────────────"
    run preview     ./tools/check/check_preview.sh
    run keymap-ui   ./tools/check/check_keymap_ui.sh
    run picker-ui   ./tools/check/check_picker_ui.sh
    run password-ui ./tools/check/check_password_ui.sh
    run tab-subsets python3 tools/check/check_tab_subsets.py
    for b in oledpad modular mystery6x6 proto2x2; do
        run "export/$b" ./tools/check/check_export.sh "$b"
    done

    echo
    echo "── firmware build ─────────────────────────────────────"
    # The only check that links a real image. Everything above passed for a
    # long time on a tree that did not compile; check_export.sh compiles an
    # exported keymap but never links a firmware. Skips cleanly without
    # PICO_SDK_PATH and the ARM toolchain. Takes a couple of minutes.
    run build       ./tools/check/check_build.sh
fi

echo
if [ -n "$fails" ]; then
    echo "FAILED:$fails"
    exit 1
fi
echo "all checks passed"
