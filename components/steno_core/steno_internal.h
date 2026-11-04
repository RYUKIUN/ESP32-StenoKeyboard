#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "steno_core.h"

/* ---- growable string ---- */
typedef struct {
    char *p;
    size_t len, cap;
} sbuf_t;
void sb_init(sbuf_t *b);
void sb_free(sbuf_t *b);
void sb_clear(sbuf_t *b);
void sb_add(sbuf_t *b, const char *s, size_t n);
void sb_adds(sbuf_t *b, const char *s);
void sb_addc(sbuf_t *b, char c);
char *sb_take(sbuf_t *b); /* returns malloc'd NUL-terminated string, resets b */

char *sx_dup(const char *s);
char *sx_ndup(const char *s, size_t n);

/* ---- text helpers (Plover formatting.py ports) ---- */
bool sx_isspace(int c);
bool sx_isword(int c);
/* WORD_RX match at pos; returns end index or -1 */
int sx_wordrx_match(const char *s, size_t len, size_t pos);
char *sx_rightmost_word(const char *s);                 /* never NULL */
bool sx_has_word_boundary(const char *s);
char *sx_capitalize_first(const char *s);
char *sx_lower_first(const char *s);
char *sx_upper_first_word(const char *s);

/* ---- regex-lite (orthography) ---- */
typedef struct re_prog re_prog_t;
re_prog_t *re_compile(const char *pattern);              /* case-insensitive, NULL on error */
void re_free(re_prog_t *p);
/* Anchored-at-start match. caps[2*i], caps[2*i+1] = group i bounds, or -1. 10 groups max */
bool re_match(const re_prog_t *p, const char *s, int len, int *caps);

/* ---- orthography ---- */
char *steno_add_suffix(steno_dict_t *d, const char *word, const char *suffix); /* malloc'd */
void steno_ortho_init(void);
