#!/usr/bin/env python3
"""
check_pins.py — find two features assigned to the same GPIO on one firmware.

A pin collision does not fail to build. It produces hardware that half works:
a display that dies when the console prints, a matrix column that reads as
pressed whenever the IR receiver sees light. This catches it at source.

    python3 tools/check/check_pins.py
"""
import re, sys, glob, os

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))

def defines(text):
    out = {}
    for m in re.finditer(r'^\s*#\s*define\s+(\w+)\s+(.+?)\s*(?://.*)?$', text, re.M):
        out[m.group(1)] = m.group(2).strip()
    return out

def pins_from(name, val):
    if re.fullmatch(r'\{[^}]*\}', val):
        return [int(x) for x in re.findall(r'\d+', val)]
    if re.fullmatch(r'\d+', val):
        return [int(val)]
    return []

# Single-pin defines worth checking, and which feature each belongs to.
SINGLE = {
    'SPLIT_UART_TX_PIN':'split bus', 'SPLIT_UART_RX_PIN':'split bus',
    'OLED_SDA_PIN':'oled', 'OLED_SCL_PIN':'oled',
    'IR_TX_PIN':'ir/rf', 'RF_TX_PIN':'ir/rf', 'IR_RX_PIN':'ir/rf',
    'RF_RX_PIN':'ir/rf', 'IR_RX_PWR_PIN':'ir/rf',
}
MULTI = {'MATRIX_ROW_PINS':'matrix rows', 'MATRIX_COL_PINS':'matrix cols'}

# Pins the platform or the build claims regardless of the board.
RESERVED = {23:'cyw43', 24:'cyw43', 25:'cyw43', 29:'cyw43'}

def check(label, texts, stdio_uart0, claim_remotes):
    d = {}
    for t in texts: d.update(defines(t))
    owner = {}
    problems = []

    if stdio_uart0:
        owner[0] = 'console uart (stdio)'
        owner[1] = 'console uart (stdio)'
    for p, who in RESERVED.items():
        owner[p] = who

    for name, feat in list(SINGLE.items()) + list(MULTI.items()):
        if name not in d: continue
        # A module firmware links no IR/RF code, so it does not claim those
        # pins; only the primary does, and only when remotes are built at all.
        if feat == 'ir/rf' and not claim_remotes: continue
        for p in pins_from(name, d[name]):
            if p in owner and owner[p] != feat:
                problems.append("GP%d: %s vs %s" % (p, owner[p], feat))
            owner[p] = feat

    for m in re.finditer(r'ENCODER\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\)',
                         "\n".join(texts)):
        for g in m.groups():
            if not g.isdigit(): continue
            p = int(g)
            if p in owner and owner[p] != 'encoder':
                problems.append("GP%d: %s vs encoder" % (p, owner[p]))
            owner[p] = 'encoder'

    status = "OK" if not problems else "COLLISION"
    print("%-28s %s" % (label, status))
    for pr in sorted(set(problems)):
        print("    " + pr)
    return len(problems)

def main():
    cfg = open(os.path.join(ROOT, 'include', 'config.h')).read()
    bad = 0
    for kb in sorted(glob.glob(os.path.join(ROOT, 'keyboards', '*'))):
        if not os.path.isdir(kb): continue
        name = os.path.basename(kb)
        hdr = os.path.join(kb, 'keyboard.h')
        if not os.path.exists(hdr): continue
        rules = os.path.join(kb, 'rules.cmake')
        remotes = True
        if os.path.exists(rules):
            r = open(rules).read()
            if re.search(r'set\(\s*ENABLE_REMOTES\s+OFF', r): remotes = False

        mods = sorted(glob.glob(os.path.join(kb, 'modules', 'module*.h')))
        if mods:
            for m in mods:
                mid = re.search(r'module(\d+)', m).group(1)
                # stdio is on uart0 for the primary and every module build.
                bad += check("%s / module %s" % (name, mid),
                             [cfg, open(hdr).read(), open(m).read()],
                             True, remotes and mid == '0')
        else:
            bad += check(name, [cfg, open(hdr).read()], True, remotes)
    print("\n%s" % ("PIN COLLISIONS FOUND" if bad else "no pin collisions"))
    return 1 if bad else 0

sys.exit(main())
