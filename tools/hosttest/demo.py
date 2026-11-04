#!/usr/bin/env python3
"""Try the steno engine on your PC, no hardware required.

    python tools/hosttest/demo.py TH- KWEUBG PWROUPB TPOBGS      -> "this quick brown fox"
    python tools/hosttest/demo.py                               -> interactive: type one stroke per line

Needs only gcc and Python 3. Strokes use standard steno notation (RTFCRE), e.g. KAT, TKOG, -G, *.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from steno_keys import parse_steno  # noqa: E402

BIN = os.path.join(HERE, "hostt")
DICT = os.path.join(HERE, "..", "..", "dict", "dict.bin")


def ensure_binary():
    if not os.path.exists(BIN):
        subprocess.run([os.path.join(HERE, "build.sh")], check=True)


def translate(strokes):
    """List of stroke strings -> text produced by the engine."""
    masks = "".join("%x\n" % parse_steno(s)[0] for s in strokes)
    p = subprocess.run([BIN, DICT], input=masks, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError("engine failed: " + p.stderr[:300])
    return p.stdout


def main():
    ensure_binary()
    if len(sys.argv) > 1:
        print(translate(sys.argv[1:]).strip())
        return
    print("Type one stroke per line (e.g. KAT, TKOG, -G, *). Empty line quits.")
    strokes = []
    while True:
        try:
            line = input("stroke> ").strip()
        except EOFError:
            break
        if not line:
            break
        try:
            strokes.append(line)
            print("  text:", repr(translate(strokes)))
        except ValueError as e:
            strokes.pop()
            print("  invalid stroke:", e)


if __name__ == "__main__":
    main()
