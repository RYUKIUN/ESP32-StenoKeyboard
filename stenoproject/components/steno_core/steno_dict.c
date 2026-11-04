/* Reader for the dictionary blob produced by tools/convert_dict.py */
#include <stdlib.h>
#include <string.h>

#include "steno_internal.h"

#define HEADER_SIZE 64
#define DIR_ENTRY 20
#define CACHE_LINES 4
#define BLOCK_CAP 6144   /* converter keeps raw blocks < 5 KB */

typedef struct {
    int block;           /* -1 = empty */
    uint32_t stamp;
    uint16_t len;
    uint8_t *buf;
} cache_line_t;

struct steno_dict {
    steno_blob_t blob;
    uint32_t entries, blocks, dir_off, blocks_off, words_off, words_size;
    uint32_t slot_count;
    uint64_t seed;
    int longest;
    uint32_t tick;
    cache_line_t cache[CACHE_LINES];
};

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static uint32_t rd24be(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

steno_dict_t *steno_dict_open(const steno_blob_t *blob)
{
    if (!blob || !blob->read || blob->size < HEADER_SIZE) return NULL;
    const uint8_t *h = blob->read(blob->ctx, 0, HEADER_SIZE);
    if (!h || memcmp(h, "STNO", 4) != 0 || rd16(h + 4) != 1) return NULL;
    steno_dict_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->blob = *blob;
    d->entries = rd32(h + 8);
    d->blocks = rd32(h + 12);
    d->dir_off = rd32(h + 16);
    d->blocks_off = rd32(h + 20);
    d->words_off = rd32(h + 24);
    d->words_size = rd32(h + 28);
    d->longest = h[0x20];
    d->slot_count = rd32(h + 0x24);
    d->seed = (uint64_t)rd32(h + 0x28) | (uint64_t)rd32(h + 0x2C) << 32;
    for (int i = 0; i < CACHE_LINES; i++) {
        d->cache[i].block = -1;
        d->cache[i].buf = malloc(BLOCK_CAP);
        if (!d->cache[i].buf) { steno_dict_close(d); return NULL; }
    }
    if (d->longest < 1 || d->longest > STENO_MAX_STROKES) { steno_dict_close(d); return NULL; }
    return d;
}

void steno_dict_close(steno_dict_t *d)
{
    if (!d) return;
    for (int i = 0; i < CACHE_LINES; i++) free(d->cache[i].buf);
    free(d);
}

int steno_dict_longest_key(const steno_dict_t *d) { return d->longest; }
uint32_t steno_dict_entry_count(const steno_dict_t *d) { return d->entries; }

/* ---------------------------------------------------------------- LZ4 */
static int lz4_decode(const uint8_t *src, int slen, uint8_t *dst, int dcap)
{
    const uint8_t *ip = src, *iend = src + slen;
    uint8_t *op = dst, *oend = dst + dcap;
    while (ip < iend) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned s;
            do { if (ip >= iend) return -1; s = *ip++; lit += s; } while (s == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op)) return -1;
        memcpy(op, ip, lit);
        op += lit; ip += lit;
        if (ip >= iend) break;
        if (iend - ip < 2) return -1;
        size_t off = (size_t)ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return -1;
        size_t ml = tok & 15;
        if (ml == 15) {
            unsigned s;
            do { if (ip >= iend) return -1; s = *ip++; ml += s; } while (s == 255);
        }
        ml += 4;
        if (ml > (size_t)(oend - op)) return -1;
        const uint8_t *m = op - off;
        while (ml--) *op++ = *m++;
    }
    return (int)(op - dst);
}

/* ------------------------------------------------------------- blocks */
static int cmp_keys(const steno_stroke_t *a, int na, const steno_stroke_t *b, int nb)
{
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return na < nb ? -1 : (na > nb ? 1 : 0);
}

static const uint8_t *dir_entry(steno_dict_t *d, uint32_t i)
{
    return d->blob.read(d->blob.ctx, d->dir_off + i * DIR_ENTRY, DIR_ENTRY);
}

static const cache_line_t *load_block(steno_dict_t *d, int idx)
{
    cache_line_t *victim = &d->cache[0];
    for (int i = 0; i < CACHE_LINES; i++) {
        if (d->cache[i].block == idx) { d->cache[i].stamp = ++d->tick; return &d->cache[i]; }
        if (d->cache[i].block < 0 || d->cache[i].stamp < victim->stamp) victim = &d->cache[i];
    }
    const uint8_t *de = dir_entry(d, (uint32_t)idx);
    if (!de) return NULL;
    uint32_t off = rd32(de), clen = rd16(de + 4), rlen = rd16(de + 6);
    if (rlen > BLOCK_CAP) return NULL;
    const uint8_t *src = d->blob.read(d->blob.ctx, d->blocks_off + off, clen);
    if (!src) return NULL;
    if (clen == rlen) {
        memcpy(victim->buf, src, rlen);
    } else if (lz4_decode(src, (int)clen, victim->buf, (int)rlen) != (int)rlen) {
        victim->block = -1;
        return NULL;
    }
    victim->block = idx;
    victim->len = (uint16_t)rlen;
    victim->stamp = ++d->tick;
    return victim;
}

char *steno_dict_lookup(steno_dict_t *d, const steno_stroke_t *key, int n)
{
    if (!d || n < 1 || n > STENO_MAX_STROKES || d->blocks == 0) return NULL;

    /* last block whose first key <= key */
    int lo = 0, hi = (int)d->blocks - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *de = dir_entry(d, (uint32_t)mid);
        if (!de) return NULL;
        int fn = de[8];
        steno_stroke_t fk[3];
        for (int i = 0; i < fn && i < 3; i++) fk[i] = rd24be(de + 9 + 3 * i);
        if (cmp_keys(fk, fn, key, n) <= 0) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (found < 0) return NULL;

    const cache_line_t *cl = load_block(d, found);
    if (!cl) return NULL;
    const uint8_t *p = cl->buf, *end = cl->buf + cl->len;
    steno_stroke_t cur[STENO_MAX_STROKES];
    int ncur = 0;
    while (p < end) {
        unsigned b0 = *p++;
        int sh = b0 >> 4, nw = b0 & 15;
        if (sh > ncur || sh + nw > STENO_MAX_STROKES || p + 3 * nw > end) return NULL;
        for (int i = 0; i < nw; i++) { cur[sh + i] = rd24be(p); p += 3; }
        ncur = sh + nw;
        uint32_t vlen = 0;
        int shift = 0;
        for (;;) {
            if (p >= end) return NULL;
            unsigned c = *p++;
            vlen |= (uint32_t)(c & 0x7F) << shift;
            if (!(c & 0x80)) break;
            shift += 7;
        }
        if (p + vlen > end) return NULL;
        int c = cmp_keys(cur, ncur, key, n);
        if (c == 0) return sx_ndup((const char *)p, vlen);
        if (c > 0) return NULL;
        p += vlen;
    }
    return NULL;
}

/* ------------------------------------------------------- word filter */
static uint64_t fnv1a64(const uint8_t *s, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 0x100000001B3ULL;
    return h;
}
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

int steno_dict_word_class(steno_dict_t *d, const char *word, size_t len)
{
    if (!d || d->slot_count < 3 || len == 0) return -1;
    uint32_t bl = d->slot_count / 3;
    uint64_t h = splitmix64(fnv1a64((const uint8_t *)word, len) ^ d->seed);
    uint32_t s0 = (uint32_t)(((h & 0xFFFFFFFFULL) * bl) >> 32);
    uint32_t s1 = (uint32_t)(((rotl64(h, 21) & 0xFFFFFFFFULL) * bl) >> 32) + bl;
    uint32_t s2 = (uint32_t)(((rotl64(h, 42) & 0xFFFFFFFFULL) * bl) >> 32) + 2 * bl;
    uint32_t v = 0;
    uint32_t idx[3] = { s0, s1, s2 };
    for (int i = 0; i < 3; i++) {
        const uint8_t *p = d->blob.read(d->blob.ctx, d->words_off + 2 * idx[i], 2);
        if (!p) return -1;
        v ^= rd16(p);
    }
    uint32_t fp = (uint32_t)((h ^ (h >> 32)) & 0xFFF);
    if ((v & 0xFFF) == fp && (v >> 12) <= 8) return (int)(v >> 12);
    return -1;
}
