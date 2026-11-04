#!/usr/bin/env python3
"""Convert Plover dictionaries into the binary blob read by the steno firmware.

    pip install lz4
    python convert_dict.py                       # uses ./assets + ./user.json -> ../dict/dict.bin

Priority (like Plover): user.json > commands.json > main.json (first one wins).
Output layout: see README ("รูปแบบไฟล์พจนานุกรม")
"""
import argparse, json, os, struct, sys, unicodedata
import lz4.block
import steno_keys as sk

MAGIC = b"STNO"
VERSION = 1
HEADER_SIZE = 64
DIR_ENTRY = 20
MAX_FIRST_STROKES = 3

ASCII_MAP = {"“": '"', "”": '"', "‘": "'", "’": "'", "–": "-",
             "—": "--", "…": "...", "©": "(c)", "®": "(R)",
             "™": "(TM)", "×": "x", "÷": "/", " ": " "}


def sanitize(text):
    out = []
    for ch in text:
        if ord(ch) < 128:
            out.append(ch)
        elif ch in ASCII_MAP:
            out.append(ASCII_MAP[ch])
        else:
            d = unicodedata.normalize("NFKD", ch)
            d = "".join(c for c in d if ord(c) < 128)
            if not d:
                return None
            out.append(d)
    return "".join(out)


def varint(n):
    b = bytearray()
    while n >= 0x80:
        b.append(n & 0x7F | 0x80)
        n >>= 7
    b.append(n)
    return bytes(b)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def merge(paths):
    merged, stats = {}, {"bad_key": 0, "non_ascii_dropped": 0, "overridden": 0}
    for p in paths:  # highest priority first
        for k, v in load_json(p).items():
            try:
                key = sk.parse_steno(k)
            except ValueError:
                stats["bad_key"] += 1
                continue
            if key in merged:
                stats["overridden"] += 1
                continue
            s = sanitize(v)
            if s is None:
                stats["non_ascii_dropped"] += 1
                continue
            merged[key] = s.encode("ascii")
    return merged, stats


def build_blocks(entries, block_size):
    blocks = []  # (raw bytes, first_key)
    cur = bytearray()
    first = None
    prev = ()
    for key, val in entries:
        if cur and len(cur) >= block_size and len(key) <= MAX_FIRST_STROKES:
            blocks.append((bytes(cur), first))
            cur = bytearray()
            prev = ()
        if not cur:
            first = key
            if len(key) > MAX_FIRST_STROKES:
                raise SystemExit("internal: block starts with long key")
        sh = 0
        while sh < len(key) and sh < len(prev) and key[sh] == prev[sh]:
            sh += 1
        if not cur:
            sh = 0
        new = key[sh:]
        cur.append(sh << 4 | len(new))
        for s in new:
            cur += s.to_bytes(3, "big")
        cur += varint(len(val)) + val
        prev = key
    if cur:
        blocks.append((bytes(cur), first))
    return blocks


# ---------------------------------------------------------------- xor filter
M64 = (1 << 64) - 1


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & M64
    return h


def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & M64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


def rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def filter_hash(word, seed):
    return splitmix64(fnv1a64(word) ^ seed)


def filter_slots(h, blocklen):
    h0 = ((h & 0xFFFFFFFF) * blocklen) >> 32
    h1 = ((rotl(h, 21) & 0xFFFFFFFF) * blocklen) >> 32
    h2 = ((rotl(h, 42) & 0xFFFFFFFF) * blocklen) >> 32
    return h0, h1 + blocklen, h2 + 2 * blocklen


def filter_fp(h):
    return (h ^ (h >> 32)) & 0xFFF


def build_filter(words):
    """words: list of (bytes, class). Returns (slots_bytes, seed, slot_count)."""
    n = len(words)
    slot_count = int(n * 1.23) + 32
    slot_count -= slot_count % 3
    blocklen = slot_count // 3
    seed = 0x5EED
    while True:
        count = [0] * slot_count
        xh = [0] * slot_count
        hs = []
        for w, c in words:
            h = filter_hash(w, seed)
            hs.append(h)
            for s in filter_slots(h, blocklen):
                count[s] += 1
                xh[s] ^= h
        queue = [i for i in range(slot_count) if count[i] == 1]
        stack = []
        while queue:
            i = queue.pop()
            if count[i] != 1:
                continue
            h = xh[i]
            stack.append((h, i))
            for s in filter_slots(h, blocklen):
                count[s] -= 1
                xh[s] ^= h
                if s != i and count[s] == 1:
                    queue.append(s)
        if len(stack) == n:
            break
        seed += 1
        print(f"  xor filter: peeling failed, retry with seed {seed:#x}")
    cls = {}
    for (w, c), h in zip(words, hs):
        cls[h] = c
    B = [0] * slot_count
    for h, i in reversed(stack):
        v = filter_fp(h) | (cls[h] << 12)
        s0, s1, s2 = filter_slots(h, blocklen)
        B[i] = v ^ B[s0] ^ B[s1] ^ B[s2] ^ B[i]  # B[i] is 0 here, others already set
    return struct.pack("<%dH" % slot_count, *B), seed, slot_count


def filter_query(B, slot_count, seed, word):
    blocklen = slot_count // 3
    h = filter_hash(word, seed)
    s0, s1, s2 = filter_slots(h, blocklen)
    v = B[s0] ^ B[s1] ^ B[s2]
    if (v & 0xFFF) == filter_fp(h) and (v >> 12) <= 8:
        return v >> 12
    return -1


def load_wordlist(path):
    words = []
    with open(path, encoding="utf-8") as f:
        toks = f.read().split()
    for w, c in zip(toks[0::2], toks[1::2]):
        if not w.isascii():
            continue
        words.append((w.encode("ascii"), int(c)))
    classes = sorted({c for _, c in words})
    assert len(classes) <= 9, classes
    cmap = {c: i for i, c in enumerate(classes)}
    return [(w, cmap[c]) for w, c in words], classes


# ---------------------------------------------------------------------- main
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--user", default=os.path.join(here, "user.json"))
    ap.add_argument("--commands", default=os.path.join(here, "assets", "commands.json"))
    ap.add_argument("--main", default=os.path.join(here, "assets", "main.json"))
    ap.add_argument("--wordlist", default=os.path.join(here, "assets", "american_english_words.txt"))
    ap.add_argument("--out", default=os.path.join(here, "..", "dict", "dict.bin"))
    ap.add_argument("--block-size", type=int, default=4096)
    ap.add_argument("--max-size", type=int, default=0, help="fail if output is larger (bytes)")
    ap.add_argument("--extra", action="append", default=[], help="extra JSON dictionaries (lowest priority)")
    a = ap.parse_args()

    paths = [p for p in (a.user, a.commands, a.main) + tuple(a.extra) if p and os.path.exists(p)]
    print("dictionaries:", ", ".join(os.path.basename(p) for p in paths))
    merged, st = merge(paths)
    print(f"entries: {len(merged)}  (overridden {st['overridden']}, bad keys {st['bad_key']}, "
          f"dropped non-ASCII {st['non_ascii_dropped']})")
    entries = sorted(merged.items())
    longest = max(len(k) for k, _ in entries)

    blocks = build_blocks(entries, a.block_size)
    comp = []
    for raw, first in blocks:
        c = lz4.block.compress(raw, mode="high_compression", compression=12, store_size=False)
        if len(c) >= len(raw):
            c = raw
        comp.append(c)
    assert all(len(r) < 65536 for r, _ in blocks)

    dir_bytes = bytearray()
    blob = bytearray()
    for (raw, first), c in zip(blocks, comp):
        fk = b"".join(s.to_bytes(3, "big") for s in first).ljust(9, b"\0")
        dir_bytes += struct.pack("<IHHB", len(blob), len(c), len(raw), len(first)) + fk + b"\0" * 2
        blob += c
    dir_off = HEADER_SIZE
    blocks_off = dir_off + len(dir_bytes)
    words_off = (blocks_off + len(blob) + 3) & ~3

    wl, classes = load_wordlist(a.wordlist)
    print(f"wordlist: {len(wl)} words, weight classes {classes}")
    fbytes, seed, slot_count = build_filter(wl)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<HHIIIIII", header, 4, VERSION, 0, len(entries), len(blocks),
                     dir_off, blocks_off, words_off, len(fbytes))
    header[0x20] = longest
    struct.pack_into("<IQ", header, 0x24, slot_count, seed)
    out = bytes(header) + bytes(dir_bytes) + bytes(blob)
    out += b"\0" * (words_off - len(out)) + fbytes
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(out)
    print(f"blocks: {len(blocks)}  dict data {len(blob)/1e6:.2f} MB  filter {len(fbytes)/1e6:.2f} MB")
    print(f"wrote {a.out}  ({len(out)} bytes = {len(out)/1048576:.2f} MiB)")
    if a.max_size and len(out) > a.max_size:
        raise SystemExit(f"ERROR: output {len(out)} > max {a.max_size}")


if __name__ == "__main__":
    main()
