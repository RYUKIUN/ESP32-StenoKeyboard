#!/usr/bin/env python3
"""Fast regression test (no Plover needed). Run after tools/hosttest/build.sh."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo  # noqa: E402

CASES = [
    ("plain words",           "TH- KWEUBG PWROUPB TPOBGS", " this quick brown fox"),
    ("suffix + orthography",  "TKOG -G",                   " dogging"),
    ("plural key",            "KAT -Z",                    " cats"),
    ("undo (*)",              "KAT TKOG *",                " cat"),
    ("numbers",               "#TPH",                      " 234"),
    ("fingerspelling",        "A* PW* KR*",                " abc"),
    ("retro: delete space",   "KAT TKOG TK*FS",            " catdog"),
    ("keyboard shortcut",     "KO*P",                      "<Control_L(c)>"),
    ("suspend / resume",      "PHRO*F KAT PHRO*PB KAT",    " cat"),
]

failed = 0
for name, strokes, want in CASES:
    got = demo.translate(strokes.split())
    ok = got == want
    failed += not ok
    print(("PASS" if ok else "FAIL"), f"{name:22} {strokes!r} -> {got!r}" + ("" if ok else f"  (expected {want!r})"))
print(f"\n{len(CASES) - failed}/{len(CASES)} passed")
sys.exit(1 if failed else 0)
