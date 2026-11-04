#include <stdlib.h>
#include <string.h>

#include "steno_internal.h"

/* ---------------------------------------------------------------- sbuf */
void sb_init(sbuf_t *b) { b->p = NULL; b->len = b->cap = 0; }
void sb_free(sbuf_t *b) { free(b->p); sb_init(b); }
void sb_clear(sbuf_t *b) { b->len = 0; if (b->p) b->p[0] = 0; }

static void sb_reserve(sbuf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 32;
    while (nc < b->len + extra + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) abort();
    b->p = np;
    b->cap = nc;
}

void sb_add(sbuf_t *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
void sb_adds(sbuf_t *b, const char *s) { sb_add(b, s, strlen(s)); }
void sb_addc(sbuf_t *b, char c) { sb_add(b, &c, 1); }

char *sb_take(sbuf_t *b)
{
    if (!b->p) { char *e = malloc(1); if (!e) abort(); e[0] = 0; return e; }
    char *r = b->p;
    sb_init(b);
    return r;
}

char *sx_ndup(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r) abort();
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}
char *sx_dup(const char *s) { return sx_ndup(s, strlen(s)); }

/* ----------------------------------------------------------- char class */
bool sx_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
bool sx_isword(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool isdig(int c) { return c >= '0' && c <= '9'; }

/* WORD_RX = (?:\d+(?:[.,]\d+)+|['\w]+[-\w']*|[^\w\s]+)\s*   */
int sx_wordrx_match(const char *s, size_t len, size_t pos)
{
    size_t p = pos, q;
    int end = -1;
    if (pos >= len) return -1;
    /* alt 1 */
    if (isdig((unsigned char)s[p])) {
        q = p;
        while (q < len && isdig((unsigned char)s[q])) q++;
        int groups = 0;
        for (;;) {
            if (q + 1 < len && (s[q] == '.' || s[q] == ',') && isdig((unsigned char)s[q + 1])) {
                q++;
                while (q < len && isdig((unsigned char)s[q])) q++;
                groups++;
            } else break;
        }
        if (groups > 0) end = (int)q;
    }
    /* alt 2 */
    if (end < 0 && (sx_isword((unsigned char)s[p]) || s[p] == '\'')) {
        q = p + 1;
        while (q < len && (sx_isword((unsigned char)s[q]) || s[q] == '-' || s[q] == '\'')) q++;
        end = (int)q;
    }
    /* alt 3 */
    if (end < 0 && !sx_isword((unsigned char)s[p]) && !sx_isspace((unsigned char)s[p])) {
        q = p;
        while (q < len && !sx_isword((unsigned char)s[q]) && !sx_isspace((unsigned char)s[q])) q++;
        end = (int)q;
    }
    if (end < 0) return -1;
    while ((size_t)end < len && sx_isspace((unsigned char)s[end])) end++;
    return end;
}

char *sx_rightmost_word(const char *s)
{
    size_t len = strlen(s), pos = 0;
    int ls = -1, le = -1;
    while (pos < len) {
        int e = sx_wordrx_match(s, len, pos);
        if (e < 0) { pos++; continue; }
        ls = (int)pos; le = e; pos = (size_t)e;
    }
    if (ls < 0) return sx_dup("");
    if (sx_isspace((unsigned char)s[le - 1])) return sx_dup("");
    return sx_ndup(s + ls, (size_t)(le - ls));
}

bool sx_has_word_boundary(const char *s)
{
    size_t len = strlen(s), pos = 0;
    int count = 0;
    if (!len) return false;
    if (sx_isspace((unsigned char)s[0]) || sx_isspace((unsigned char)s[len - 1])) return true;
    while (pos < len) {
        int e = sx_wordrx_match(s, len, pos);
        if (e < 0) { pos++; continue; }
        count++;
        pos = (size_t)e;
    }
    return count > 1;
}

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int lo(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

char *sx_capitalize_first(const char *s)
{
    char *r = sx_dup(s);
    if (r[0]) r[0] = (char)up((unsigned char)r[0]);
    return r;
}
char *sx_lower_first(const char *s)
{
    char *r = sx_dup(s);
    if (r[0]) r[0] = (char)lo((unsigned char)r[0]);
    return r;
}
char *sx_upper_first_word(const char *s)
{
    size_t len = strlen(s);
    char *r = sx_dup(s);
    int e = sx_wordrx_match(s, len, 0);
    if (e < 0) return r;
    for (int i = 0; i < e; i++) r[i] = (char)up((unsigned char)r[i]);
    return r;
}

/* ---------------------------------------------------- stroke -> RTFCRE */
static const char KEY_LETTER[STENO_KEY_COUNT + 1] = "#STKPWHRAO*EUFRPBLGTSDZ";
static const char KEY_DIGIT[STENO_KEY_COUNT] = {
    0, '1', '2', 0, '3', 0, '4', 0, '5', '0', 0, 0, 0, '6', 0, '7', 0, '8', 0, '9', 0, 0, 0,
};

void steno_stroke_to_str(steno_stroke_t m, char *out)
{
    bool num = m & 1, converted = false, implicit = false, right = false;
    char *o = out;
    for (int i = 1; i <= 9; i++)
        if (m >> i & 1) {
            if (num && KEY_DIGIT[i]) { *o++ = KEY_DIGIT[i]; converted = true; }
            else *o++ = KEY_LETTER[i];
        }
    for (int i = 8; i <= 12; i++) if (m >> i & 1) implicit = true;
    for (int i = 13; i <= 22; i++) if (m >> i & 1) right = true;
    if (right && !implicit) *o++ = '-';
    for (int i = 10; i <= 22; i++)
        if (m >> i & 1) {
            if (num && KEY_DIGIT[i]) { *o++ = KEY_DIGIT[i]; converted = true; }
            else *o++ = KEY_LETTER[i];
        }
    *o = 0;
    if (num && !converted) {
        memmove(out + 1, out, (size_t)(o - out) + 1);
        out[0] = '#';
    }
}
