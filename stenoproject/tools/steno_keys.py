"""Standalone steno key helpers (no Plover dependency).

A stroke is a 23-bit mask. Bit i corresponds to KEYS[i] (English Stenotype order),
which is also the bit order used by the firmware (steno_core/include/steno_types.h).
"""

KEYS = ("#", "S-", "T-", "K-", "P-", "W-", "H-", "R-", "A-", "O-", "*",
        "-E", "-U", "-F", "-R", "-P", "-B", "-L", "-G", "-T", "-S", "-D", "-Z")
LETTERS = "#STKPWHRAO*EUFRPBLGTSDZ"
NUMBER_KEY = 0
IMPLICIT_HYPHEN = (8, 9, 10, 11, 12)          # A- O- * -E -U
FIRST_RIGHT = 13                              # -F
# digit char -> key index
DIGIT_TO_KEY = {"1": 1, "2": 2, "3": 4, "4": 6, "5": 8, "0": 9,
                "6": 13, "7": 15, "8": 17, "9": 19}
KEY_TO_DIGIT = {v: k for k, v in DIGIT_TO_KEY.items()}

KEY_INDEX = {name: i for i, name in enumerate(KEYS)}


def parse_stroke(s: str) -> int:
    """RTFCRE stroke string -> mask. Raises ValueError on invalid input."""
    mask = 0
    pointer = 1
    number = False
    for ch in s:
        if ch == "#":
            number = True
            mask |= 1
        elif ch == "-":
            pointer = max(pointer, FIRST_RIGHT)
        elif ch in DIGIT_TO_KEY:
            idx = DIGIT_TO_KEY[ch]
            if idx < pointer:
                raise ValueError(f"bad stroke {s!r}")
            number = True
            mask |= (1 << idx) | 1
            pointer = idx + 1
        else:
            idx = -1
            for i in range(pointer, 23):
                if LETTERS[i] == ch:
                    idx = i
                    break
            if idx < 0:
                raise ValueError(f"bad stroke {s!r}")
            mask |= 1 << idx
            pointer = idx + 1
    del number
    return mask


def format_stroke(mask: int) -> str:
    """Mask -> RTFCRE string (same rules as Plover's English Stenotype)."""
    num = bool(mask & 1)
    converted = False
    out = []
    for i in range(1, 10):
        if mask >> i & 1:
            if num and i in KEY_TO_DIGIT:
                out.append(KEY_TO_DIGIT[i]); converted = True
            else:
                out.append(LETTERS[i])
    implicit = any(mask >> i & 1 for i in IMPLICIT_HYPHEN)
    right = any(mask >> i & 1 for i in range(FIRST_RIGHT, 23))
    if right and not implicit:
        out.append("-")
    for i in range(10, 23):
        if mask >> i & 1:
            if num and i in KEY_TO_DIGIT:
                out.append(KEY_TO_DIGIT[i]); converted = True
            else:
                out.append(LETTERS[i])
    s = "".join(out)
    if num and not converted:
        s = "#" + s
    return s


def parse_steno(key: str):
    """'KAT/-G' -> tuple of masks"""
    return tuple(parse_stroke(p) for p in key.split("/"))
