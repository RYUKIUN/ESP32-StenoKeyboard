/* Port of plover/orthography.py (add_suffix) using the regex-lite engine and the wordlist filter. */
#include <stdlib.h>
#include <string.h>

#include "steno_internal.h"

typedef struct { const char *pattern, *replacement; re_prog_t *prog; } rule_t;

static rule_t s_rules[] = {
#include "steno_ortho_rules.inc"
};
#define NRULES (sizeof(s_rules) / sizeof(s_rules[0]))
static bool s_ready;

void steno_ortho_init(void)
{
    if (s_ready) return;
    for (size_t i = 0; i < NRULES; i++) {
        s_rules[i].prog = re_compile(s_rules[i].pattern);
        if (!s_rules[i].prog) abort();
    }
    s_ready = true;
}

/* expand \1..\9 in replacement */
static char *expand(const char *repl, const char *s, const int *caps)
{
    sbuf_t b;
    sb_init(&b);
    for (const char *r = repl; *r; r++) {
        if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
            int g = r[1] - '0';
            r++;
            if (caps[2 * g] >= 0 && caps[2 * g + 1] >= 0)
                sb_add(&b, s + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]));
        } else sb_addc(&b, *r);
    }
    return sb_take(&b);
}

typedef struct { char *text; int cls; } cand_t;
#define MAX_CAND 48

static void add_cand(cand_t *c, int *n, char *text, int cls)
{
    if (*n < MAX_CAND) { c[*n].text = text; c[*n].cls = cls; (*n)++; }
    else free(text);
}

/* make_candidates_from_rules; check==NULL means no dictionary check */
static void make_candidates(steno_dict_t *d, const char *word, const char *suffix, bool check,
                            cand_t *c, int *n)
{
    sbuf_t in;
    sb_init(&in);
    sb_adds(&in, word);
    sb_adds(&in, " ^ ");
    sb_adds(&in, suffix);
    for (size_t i = 0; i < NRULES; i++) {
        int caps[22];
        if (re_match(s_rules[i].prog, in.p, (int)in.len, caps)) {
            char *e = expand(s_rules[i].replacement, in.p, caps);
            int cls = check ? steno_dict_word_class(d, e, strlen(e)) : 0;
            if (!check || cls >= 0) add_cand(c, n, e, cls);
            else free(e);
        }
    }
    sb_free(&in);
}

static char *add_suffix_core(steno_dict_t *d, const char *word, const char *suffix)
{
    cand_t c[MAX_CAND];
    int n = 0;
    const char *alias = NULL;
    if (!strcmp(suffix, "able")) alias = "ible";
    else if (!strcmp(suffix, "ability")) alias = "ibility";
    if (alias) make_candidates(d, word, alias, true, c, &n);

    sbuf_t simple;
    sb_init(&simple);
    sb_adds(&simple, word);
    sb_adds(&simple, suffix);
    int sc = steno_dict_word_class(d, simple.p, simple.len);
    if (sc >= 0) add_cand(c, &n, sx_ndup(simple.p, simple.len), sc);

    make_candidates(d, word, suffix, true, c, &n);

    if (n > 0) {
        /* stable insertion sort by class */
        for (int i = 1; i < n; i++) {
            cand_t t = c[i];
            int j = i - 1;
            while (j >= 0 && c[j].cls > t.cls) { c[j + 1] = c[j]; j--; }
            c[j + 1] = t;
        }
        char *r = c[0].text;
        for (int i = 1; i < n; i++) free(c[i].text);
        sb_free(&simple);
        return r;
    }
    make_candidates(d, word, suffix, false, c, &n);
    if (n > 0) {
        char *r = c[0].text;
        for (int i = 1; i < n; i++) free(c[i].text);
        sb_free(&simple);
        return r;
    }
    return sb_take(&simple);
}

char *steno_add_suffix(steno_dict_t *d, const char *word, const char *suffix)
{
    steno_ortho_init();
    const char *sp = strchr(suffix, ' ');
    char *head = sp ? sx_ndup(suffix, (size_t)(sp - suffix)) : sx_dup(suffix);
    char *expanded = add_suffix_core(d, word, head);
    sbuf_t out;
    sb_init(&out);
    sb_adds(&out, expanded);
    if (sp) sb_adds(&out, sp);
    free(expanded);
    free(head);
    return sb_take(&out);
}
