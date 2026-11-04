/*
 * Plover translator + formatter, ported to C.
 * References: plover translation, formatting, meta and macro modules (Python)
 * Not supported (not used by Plover's default dictionaries): {=conditional}, {MODE:...},
 * {*(currency)}, prefix strokes, space-after placement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "steno_internal.h"

/* ================================================================= actions */
enum { CASE_NONE = 0, CASE_CAP_FIRST_WORD, CASE_LOWER_FIRST_CHAR, CASE_UPPER_FIRST_WORD };

typedef struct {
    bool prev_attach, glue, orthography, upper_carry, word_is_finished, next_attach;
    uint8_t next_case;
    char *word;          /* NULL = None */
    char *prev_replace;  /* NULL = "" */
    char *text;          /* NULL = None */
    char *combo;
    char *command;
} action_t;

static action_t *act_alloc(void)
{
    action_t *a = calloc(1, sizeof(*a));
    if (!a) abort();
    a->orthography = true;
    a->word_is_finished = true;
    return a;
}
static void act_free(action_t *a)
{
    if (!a) return;
    free(a->word); free(a->prev_replace); free(a->text); free(a->combo); free(a->command);
    free(a);
}
/* _Action.new_state() */
static void act_new_state(action_t *a, const action_t *last)
{
    a->prev_attach = last->next_attach;
}
/* _Action.copy_state() */
static void act_copy_state(action_t *a, const action_t *last)
{
    a->prev_attach = last->next_attach;
    a->glue = last->glue;
    a->orthography = last->orthography;
    a->upper_carry = last->upper_carry;
    a->word = last->word ? sx_dup(last->word) : NULL;
    a->word_is_finished = last->word_is_finished;
    a->next_attach = last->next_attach;
    a->next_case = last->next_case;
}
static bool streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}
static bool streq_empty(const char *a, const char *b) { return streq(a ? a : "", b ? b : ""); }
static bool act_eq(const action_t *a, const action_t *b)
{
    return a->prev_attach == b->prev_attach && a->glue == b->glue && a->orthography == b->orthography &&
           a->upper_carry == b->upper_carry && a->word_is_finished == b->word_is_finished &&
           a->next_attach == b->next_attach && a->next_case == b->next_case &&
           streq(a->word, b->word) && streq_empty(a->prev_replace, b->prev_replace) &&
           streq(a->text, b->text) && streq(a->combo, b->combo) && streq(a->command, b->command);
}

/* ============================================================ translations */
typedef struct translation {
    int refs;
    steno_stroke_t strokes[STENO_MAX_STROKES];
    int n;
    char *english;                 /* NULL = untranslated */
    struct translation **replaced;
    int nreplaced;
    action_t **act;                /* "formatting" */
    int nact;
    bool is_retro;
} translation_t;

static translation_t *tr_new(const steno_stroke_t *s, int n, char *english)
{
    translation_t *t = calloc(1, sizeof(*t));
    if (!t) abort();
    t->refs = 1;
    memcpy(t->strokes, s, (size_t)n * sizeof(*s));
    t->n = n;
    t->english = english;
    return t;
}
static translation_t *tr_ref(translation_t *t) { t->refs++; return t; }
static void tr_release(translation_t *t)
{
    if (!t || --t->refs > 0) return;
    for (int i = 0; i < t->nreplaced; i++) tr_release(t->replaced[i]);
    free(t->replaced);
    for (int i = 0; i < t->nact; i++) act_free(t->act[i]);
    free(t->act);
    free(t->english);
    free(t);
}
static bool tr_has_undo(const translation_t *t)
{
    if (t->nact == 0) return true;
    if (t->nreplaced) return true;
    for (int i = 0; i < t->nact; i++) {
        const action_t *a = t->act[i];
        if ((a->text && a->text[0]) || (a->prev_replace && a->prev_replace[0])) return true;
    }
    return false;
}

/* ================================================================== engine */
typedef struct { action_t **p; int n, cap; } actlist_t;
static void al_push(actlist_t *l, action_t *a)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->p = realloc(l->p, (size_t)l->cap * sizeof(*l->p));
        if (!l->p) abort();
    }
    l->p[l->n++] = a;
}

typedef struct { translation_t **p; int n, cap; } trlist_t;
static void tl_push(trlist_t *l, translation_t *t)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->p = realloc(l->p, (size_t)l->cap * sizeof(*l->p));
        if (!l->p) abort();
    }
    l->p[l->n++] = t;
}

struct steno_engine {
    steno_dict_t *dict;
    steno_output_t out;
    int undo_levels, max_len;
    trlist_t state;          /* owns a ref on each */
    translation_t *tail;
    trlist_t to_undo;        /* owns a ref on each */
    int to_do;
    bool output_enabled;
};

static const int SUFFIX_KEYS[4] = { STENO_KEY_Z_R, STENO_KEY_D_R, STENO_KEY_S_R, STENO_KEY_G_R };

/* ------------------------------------------------------------ formatting */
typedef struct {
    translation_t *const *prev;
    int nprev;
    action_t default_last;
    const action_t *last;
    actlist_t translated;
} ctx_t;

static action_t *ctx_new_action(ctx_t *c)
{
    action_t *a = act_alloc();
    act_new_state(a, c->last);
    return a;
}
static action_t *ctx_copy_last_action(ctx_t *c)
{
    action_t *a = act_alloc();
    act_copy_state(a, c->last);
    return a;
}
static void ctx_translated(ctx_t *c, action_t *a)
{
    al_push(&c->translated, a);
    c->last = a;
}

/* forward-simulated text of the context (see RetroFormatter) */
static void sim_action(sbuf_t *b, const action_t *a)
{
    if (!a->text) return;
    size_t pl = a->prev_replace ? strlen(a->prev_replace) : 0;
    if (pl) {
        if (pl > b->len) pl = b->len;
        b->len -= pl;
        if (b->p) b->p[b->len] = 0;
    }
    if (!a->prev_attach) sb_addc(b, ' ');
    sb_adds(b, a->text);
}
static char *ctx_text(const ctx_t *c)
{
    sbuf_t b;
    sb_init(&b);
    for (int i = 0; i < c->nprev; i++)
        for (int j = 0; j < c->prev[i]->nact; j++) sim_action(&b, c->prev[i]->act[j]);
    for (int j = 0; j < c->translated.n; j++) sim_action(&b, c->translated.p[j]);
    char *s = sb_take(&b);
    size_t skip = 0;
    while (s[skip] && sx_isspace((unsigned char)s[skip])) skip++;
    if (skip) memmove(s, s + skip, strlen(s + skip) + 1);
    return s;
}
static char *ctx_last_text(const ctx_t *c, size_t size)
{
    if (!size) return sx_dup("");
    char *t = ctx_text(c);
    size_t len = strlen(t);
    char *r = len > size ? sx_dup(t + len - size) : sx_dup(t);
    free(t);
    return r;
}
static char *ctx_last_word(const ctx_t *c)   /* last_words(count=1)[0] or NULL */
{
    char *t = ctx_text(c);
    size_t len = strlen(t), pos = 0;
    int ls = -1, le = -1;
    while (pos < len) {
        int e = sx_wordrx_match(t, len, pos);
        if (e < 0) { pos++; continue; }
        ls = (int)pos; le = e; pos = (size_t)e;
    }
    char *r = ls < 0 ? NULL : sx_ndup(t + ls, (size_t)(le - ls));
    free(t);
    return r;
}

static char *apply_case(const char *text, int cs)
{
    switch (cs) {
    case CASE_CAP_FIRST_WORD: return sx_capitalize_first(text);
    case CASE_LOWER_FIRST_CHAR: return sx_lower_first(text);
    case CASE_UPPER_FIRST_WORD: return sx_upper_first_word(text);
    default: return sx_dup(text);
    }
}

static void finalize_action(action_t *a, ctx_t *c)
{
    if (!a->text) return;
    if (!a->word) {
        const char *lw = NULL;
        if (a->glue && c->last->glue) lw = c->last->word;
        sbuf_t b;
        sb_init(&b);
        sb_adds(&b, lw ? lw : "");
        sb_adds(&b, a->text);
        a->word = sx_rightmost_word(b.p ? b.p : "");
        sb_free(&b);
    }
    int cs = c->last->next_case;
    if (cs == CASE_NONE && a->prev_attach && c->last->upper_carry) cs = CASE_UPPER_FIRST_WORD;
    char *t = apply_case(a->text, cs);
    free(a->text);
    a->text = t;
    if (cs == CASE_UPPER_FIRST_WORD) a->upper_carry = !sx_has_word_boundary(t);
}

static size_t common_prefix(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return i;
}

/* ---- metas ---- */
static action_t *meta_attach(steno_engine_t *e, ctx_t *c, const char *meta_in)
{
    action_t *a = ctx_new_action(c);
    size_t ml = strlen(meta_in);
    bool begin = ml > 0 && meta_in[0] == '^';
    bool end = ml > 0 && meta_in[ml - 1] == '^';
    char *meta;
    if (!(begin || end)) {
        meta = sx_dup(meta_in);
        begin = end = true;
    } else {
        size_t s = begin ? 1 : 0, en = ml - (end ? 1 : 0);
        if (begin && end && ml == 1) en = 1;   /* "^" alone: begin and end are the same char */
        meta = (en > s) ? sx_ndup(meta_in + s, en - s) : sx_dup("");
        if (begin && end && ml == 1) { free(meta); meta = sx_dup(""); }
    }
    if (begin) a->prev_attach = true;
    if (end) { a->next_attach = true; a->word_is_finished = false; }
    char *last_word = sx_dup(c->last->word ? c->last->word : "");

    bool all_space = meta[0] != 0;
    for (const char *p = meta; *p; p++) if (!sx_isspace((unsigned char)*p)) { all_space = false; break; }

    if (!meta[0]) {
        a->orthography = false;
    } else if (last_word[0] && !all_space && c->last->orthography && begin &&
               (!end || sx_has_word_boundary(meta))) {
        char *nw = steno_add_suffix(e->dict, last_word, meta);
        size_t cl = common_prefix(last_word, nw);
        size_t rl = strlen(last_word) - cl;
        a->prev_replace = ctx_last_text(c, rl);
        last_word[cl] = 0;
        char *nm = sx_dup(nw + cl);
        free(meta);
        meta = nm;
        free(nw);
    }
    a->text = meta;
    if (a->prev_attach) {
        sbuf_t b;
        sb_init(&b);
        sb_adds(&b, last_word);
        sb_adds(&b, meta);
        free(a->word);
        a->word = sx_rightmost_word(b.p ? b.p : "");
        sb_free(&b);
    }
    free(last_word);
    return a;
}

static action_t *meta_carry_capitalize(ctx_t *c, const char *meta_in)
{
    action_t *a = ctx_new_action(c);
    if (c->last->next_case == CASE_CAP_FIRST_WORD) a->next_case = CASE_CAP_FIRST_WORD;
    const char *m = meta_in;
    bool begin = m[0] == '^';
    if (begin) { m++; a->prev_attach = true; }
    m += 2; /* "~|" */
    size_t l = strlen(m);
    bool end = l > 0 && m[l - 1] == '^';
    char *text;
    if (end) { text = sx_ndup(m, l - 1); a->next_attach = true; a->word_is_finished = false; }
    else text = sx_dup(m);
    if (text[0] || begin || end) a->text = text; else free(text);
    return a;
}

static bool ieq_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        int a = (unsigned char)*s, b = (unsigned char)*prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static action_t *meta_to_action(steno_engine_t *e, ctx_t *c, const char *m)
{
    size_t l = strlen(m);
    action_t *a;
    /* 1: generic {:macro:cmdline} is not supported -> empty action */
    if (m[0] == ':' && l > 1 && m[1] != ':') return ctx_new_action(c);
    if (ieq_prefix(m, "PLOVER:")) {
        a = ctx_copy_last_action(c);
        a->command = sx_dup(m + 7);
        return a;
    }
    if (m[0] == '#') {
        a = ctx_copy_last_action(c);
        a->combo = sx_dup(m + 1);
        return a;
    }
    if (l == 1 && (m[0] == ',' || m[0] == ':' || m[0] == ';')) {
        a = ctx_new_action(c);
        a->text = sx_dup(m);
        a->prev_attach = true;
        return a;
    }
    if (l == 1 && (m[0] == '.' || m[0] == '!' || m[0] == '?')) {
        a = ctx_new_action(c);
        a->prev_attach = true;
        a->text = sx_dup(m);
        a->next_case = CASE_CAP_FIRST_WORD;
        return a;
    }
    if (!strcmp(m, "-|") || !strcmp(m, ">") || !strcmp(m, "<")) {
        a = ctx_copy_last_action(c);
        a->next_case = m[0] == '-' ? CASE_CAP_FIRST_WORD : (m[0] == '>' ? CASE_LOWER_FIRST_CHAR : CASE_UPPER_FIRST_WORD);
        return a;
    }
    if (!strcmp(m, "*-|") || !strcmp(m, "*>") || !strcmp(m, "*<")) {
        int cs = m[1] == '-' ? CASE_CAP_FIRST_WORD : (m[1] == '>' ? CASE_LOWER_FIRST_CHAR : CASE_UPPER_FIRST_WORD);
        a = ctx_copy_last_action(c);
        a->prev_attach = true;
        char *lw = ctx_last_word(c);
        if (lw) { a->prev_replace = lw; a->text = apply_case(lw, cs); }
        else a->text = sx_dup("");
        return a;
    }
    if (!strcmp(m, "$")) {
        a = ctx_copy_last_action(c);
        a->word_is_finished = true;
        return a;
    }
    if (m[0] == '=') return ctx_new_action(c);              /* conditional: unsupported */
    if (ieq_prefix(m, "MODE:")) return ctx_copy_last_action(c);
    if (m[0] == '*' && m[1] == '(' && l >= 3 && m[l - 1] == ')') return ctx_copy_last_action(c);
    if (m[0] == '&') {
        a = ctx_new_action(c);
        a->glue = true;
        a->text = sx_dup(m + 1);
        if (c->last->glue) a->prev_attach = true;
        return a;
    }
    /* carry capitalization: ^?~|.*^? */
    if ((m[0] == '~' && m[1] == '|') || (m[0] == '^' && m[1] == '~' && m[2] == '|'))
        return meta_carry_capitalize(c, m);
    if (m[0] == '^' || (l > 0 && m[l - 1] == '^')) return meta_attach(e, c, m);
    return ctx_new_action(c);
}

static char *unescape_atom(const char *s)
{
    sbuf_t b;
    sb_init(&b);
    for (; *s; s++) {
        if (s[0] == '\\' && (s[1] == '{' || s[1] == '}')) { sb_addc(&b, s[1]); s++; }
        else sb_addc(&b, *s);
    }
    return sb_take(&b);
}

static action_t *atom_to_action(steno_engine_t *e, ctx_t *c, const char *atom, size_t alen)
{
    action_t *a;
    if (alen >= 2 && atom[0] == '{' && atom[alen - 1] == '}') {
        char *inner = sx_ndup(atom + 1, alen - 2);
        char *un = unescape_atom(inner);
        free(inner);
        a = meta_to_action(e, c, un);
        free(un);
    } else {
        a = ctx_new_action(c);
        char *tmp = sx_ndup(atom, alen);
        a->text = unescape_atom(tmp);
        free(tmp);
    }
    finalize_action(a, c);
    return a;
}

static void translation_to_actions(steno_engine_t *e, const char *tr, ctx_t *c, actlist_t *out)
{
    size_t len = strlen(tr);
    bool digits = len > 0;
    for (size_t i = 0; i < len; i++) if (tr[i] < '0' || tr[i] > '9') { digits = false; break; }
    if (digits) {
        sbuf_t b;
        sb_init(&b);
        sb_addc(&b, '{'); sb_addc(&b, '&'); sb_adds(&b, tr); sb_addc(&b, '}');
        action_t *a = atom_to_action(e, c, b.p, b.len);
        sb_free(&b);
        al_push(out, a);
        ctx_translated(c, a);
    } else {
        size_t pos = 0;
        while (pos < len) {
            size_t start = pos, end;
            if (tr[pos] == '{') {
                size_t q = pos + 1;
                for (;;) {
                    if (tr[q] == '\\' && (tr[q + 1] == '{' || tr[q + 1] == '}')) q += 2;
                    else if (tr[q] && tr[q] != '{' && tr[q] != '}') q++;
                    else break;
                }
                if (tr[q] == '}') end = q + 1;
                else { pos++; continue; }
            } else {
                size_t q = pos;
                for (;;) {
                    if (tr[q] == '\\' && (tr[q + 1] == '{' || tr[q + 1] == '}')) q += 2;
                    else if (tr[q] && tr[q] != '{' && tr[q] != '}') q++;
                    else break;
                }
                end = q;
            }
            pos = end;
            /* strip(" ") */
            while (start < end && tr[start] == ' ') start++;
            while (end > start && tr[end - 1] == ' ') end--;
            if (end == start) continue;
            action_t *a = atom_to_action(e, c, tr + start, end - start);
            al_push(out, a);
            ctx_translated(c, a);
        }
    }
    if (out->n == 0) {
        action_t *a = ctx_copy_last_action(c);
        al_push(out, a);
        ctx_translated(c, a);
    }
}

static void raw_to_actions(steno_engine_t *e, steno_stroke_t stroke, ctx_t *c, actlist_t *out)
{
    char s[40];
    steno_stroke_to_str(stroke, s);
    char nodash[40];
    bool dropped = false;
    size_t k = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-' && !dropped) { dropped = true; continue; }
        nodash[k++] = *p;
    }
    nodash[k] = 0;
    bool digits = k > 0;
    for (size_t i = 0; i < k; i++) if (nodash[i] < '0' || nodash[i] > '9') { digits = false; break; }
    if (digits) { translation_to_actions(e, nodash, c, out); return; }
    action_t *a = act_alloc();
    a->text = sx_dup(s);
    a->word = sx_dup(s);
    a->prev_attach = c->last->next_attach;
    al_push(out, a);
    ctx_translated(c, a);
}

/* ------------------------------------------------------------- rendering */
typedef struct { sbuf_t replaced, appended; } tf_t;

static void tf_init(tf_t *t) { sb_init(&t->replaced); sb_init(&t->appended); }
static void tf_free(tf_t *t) { sb_free(&t->replaced); sb_free(&t->appended); }
static void tf_reset(tf_t *t) { sb_clear(&t->replaced); sb_clear(&t->appended); }

static void tf_render_action(tf_t *t, const action_t *a)
{
    size_t plen = a->prev_replace ? strlen(a->prev_replace) : 0;
    if (plen) {
        size_t appended = t->appended.len;
        if (plen > appended) {
            size_t replaced = plen - appended;
            if (replaced > t->replaced.len) {
                sb_clear(&t->replaced);
                sb_add(&t->replaced, a->prev_replace, replaced);
            } else {
                t->replaced.len -= replaced;
                t->replaced.p[t->replaced.len] = 0;
                sb_add(&t->replaced, a->prev_replace, replaced);
            }
            sb_clear(&t->appended);
        } else {
            t->appended.len -= plen;
            t->appended.p[t->appended.len] = 0;
        }
    }
    if (!a->prev_attach) sb_addc(&t->appended, ' ');
    sb_adds(&t->appended, a->text);
}

static void out_flush(steno_engine_t *e, tf_t *before, tf_t *after)
{
    const sbuf_t *rt = before->replaced.len > after->replaced.len ? &before->replaced : &after->replaced;
    sbuf_t bs, as;
    sb_init(&bs);
    sb_init(&as);
    if (rt->len) {
        sb_add(&bs, rt->p, rt->len - before->replaced.len);
        sb_add(&as, rt->p, rt->len - after->replaced.len);
    }
    if (before->appended.len) sb_add(&bs, before->appended.p, before->appended.len);
    if (after->appended.len) sb_add(&as, after->appended.p, after->appended.len);
    const char *b = bs.p ? bs.p : "", *a = as.p ? as.p : "";
    size_t common = common_prefix(b, a);
    size_t erased = bs.len - common;
    if (e->output_enabled) {
        if (erased && e->out.send_backspaces) e->out.send_backspaces(e->out.ctx, (int)erased);
        if (as.len > common && e->out.send_string) e->out.send_string(e->out.ctx, a + common, as.len - common);
    }
    sb_free(&bs);
    sb_free(&as);
    tf_reset(before);
    tf_reset(after);
}

static void engine_command(steno_engine_t *e, const char *cmd)
{
    char name[16];
    size_t i = 0;
    for (; cmd[i] && cmd[i] != ':' && i < sizeof(name) - 1; i++) {
        int ch = (unsigned char)cmd[i];
        name[i] = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    }
    name[i] = 0;
    bool old = e->output_enabled;
    if (!strcmp(name, "suspend")) e->output_enabled = false;
    else if (!strcmp(name, "resume")) e->output_enabled = true;
    else if (!strcmp(name, "toggle")) e->output_enabled = !e->output_enabled;
    if (old != e->output_enabled && e->out.on_output_enabled) e->out.on_output_enabled(e->out.ctx, e->output_enabled);
}

static void render_output(steno_engine_t *e, actlist_t *undo, actlist_t *dolist)
{
    tf_t before, after;
    tf_init(&before);
    tf_init(&after);
    for (int i = 0; i < undo->n; i++)
        if (undo->p[i]->text) tf_render_action(&before, undo->p[i]);
    for (int i = 0; i < dolist->n; i++) {
        const action_t *a = dolist->p[i];
        if (a->text) { tf_render_action(&after, a); continue; }
        out_flush(e, &before, &after);
        if (a->combo) {
            if (e->output_enabled && e->out.send_combo) e->out.send_combo(e->out.ctx, a->combo);
        } else if (a->command) engine_command(e, a->command);
    }
    out_flush(e, &before, &after);
    tf_free(&before);
    tf_free(&after);
}

/* Formatter.format(undo, do, prev) */
static void format_translations(steno_engine_t *e, translation_t **undo, int nundo, translation_t **dolist,
                                int ndo, translation_t *const *prev, int nprev)
{
    actlist_t garbage = { 0 };
    actlist_t newl = { 0 }, oldl = { 0 };
    ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (ndo) {
        ctx.prev = prev;
        ctx.nprev = nprev;
        action_t *la = NULL;
        if (nprev && prev[nprev - 1]->nact) la = prev[nprev - 1]->act[prev[nprev - 1]->nact - 1];
        if (la) ctx.last = la;
        else { ctx.default_last.orthography = true; ctx.default_last.word_is_finished = true; ctx.last = &ctx.default_last; }
        for (int i = 0; i < ndo; i++) {
            translation_t *t = dolist[i];
            for (int j = 0; j < t->nact; j++) al_push(&garbage, t->act[j]);
            free(t->act);
            t->act = NULL;
            t->nact = 0;
            actlist_t acts = { 0 };
            if (t->english && t->english[0]) translation_to_actions(e, t->english, &ctx, &acts);
            else raw_to_actions(e, t->strokes[0], &ctx, &acts);
            t->act = acts.p;
            t->nact = acts.n;
        }
        newl = ctx.translated;   /* take over */
    }
    for (int i = 0; i < nundo; i++)
        for (int j = 0; j < undo[i]->nact; j++) al_push(&oldl, undo[i]->act[j]);

    int minl = newl.n < oldl.n ? newl.n : oldl.n, i = 0;
    while (i < minl && act_eq(oldl.p[i], newl.p[i])) i++;
    actlist_t o2 = { oldl.p + i, oldl.n - i, 0 }, n2 = { newl.p + i, newl.n - i, 0 };
    render_output(e, &o2, &n2);

    for (int k = 0; k < garbage.n; k++) act_free(garbage.p[k]);
    free(garbage.p);
    free(newl.p);
    free(oldl.p);
    if (ndo) { ctx.translated.p = NULL; }
    free(ctx.default_last.word);
}

/* ================================================================ translator */
static void state_push(steno_engine_t *e, translation_t *t) { tl_push(&e->state, tr_ref(t)); }
static translation_t *state_pop(steno_engine_t *e) { return e->state.p[--e->state.n]; }

static void do_undo(steno_engine_t *e, translation_t *const *ts, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        translation_t *t = state_pop(e);
        (void)ts;
        if (e->to_do) { e->to_do--; tr_release(t); }
        else {
            /* insert at front of to_undo */
            tl_push(&e->to_undo, t);
            memmove(e->to_undo.p + 1, e->to_undo.p, (size_t)(e->to_undo.n - 1) * sizeof(translation_t *));
            e->to_undo.p[0] = t;
        }
    }
}
static void do_do(steno_engine_t *e, translation_t *const *ts, int n)
{
    for (int i = 0; i < n; i++) state_push(e, ts[i]);
    e->to_do += n;
}

static void translate_translation(steno_engine_t *e, translation_t *t)
{
    /* copy replaced pointers: do_undo releases the state's ref, replaced keeps its own */
    do_undo(e, t->replaced, t->nreplaced);
    do_do(e, &t, 1);
}
static void untranslate_translation(steno_engine_t *e, translation_t *t)
{
    do_undo(e, &t, 1);
    /* t->replaced re-enter the state; ownership moves from t to the state via incref */
    do_do(e, t->replaced, t->nreplaced);
}

static void resize_translations(steno_engine_t *e)
{
    int n = e->max_len > e->undo_levels ? e->max_len : e->undo_levels;
    int stroke_count = 0, tcount = 0;
    for (int i = e->state.n - 1; i >= 0; i--) {
        stroke_count += e->state.p[i]->n;
        tcount++;
        if (stroke_count >= n) break;
    }
    int idx = e->state.n - tcount;
    if (idx > 0) {
        translation_t *nt = tr_ref(e->state.p[idx - 1]);
        tr_release(e->tail);
        e->tail = nt;
        for (int i = 0; i < idx; i++) tr_release(e->state.p[i]);
        memmove(e->state.p, e->state.p + idx, (size_t)(e->state.n - idx) * sizeof(translation_t *));
        e->state.n -= idx;
    }
}

static void flush(steno_engine_t *e, translation_t *extra)
{
    translation_t **prev = NULL, **dolist = NULL;
    int nprev, ndo = 0;
    translation_t *tailv[1];
    if (e->to_do) {
        int pc = e->state.n - e->to_do;
        if (pc > 0) { prev = e->state.p; nprev = pc; }
        else if (e->tail) { tailv[0] = e->tail; prev = tailv; nprev = 1; }
        else { prev = NULL; nprev = 0; }
        ndo = e->to_do;
        dolist = e->state.p + (e->state.n - e->to_do);
    } else {
        if (e->state.n > 0) { prev = e->state.p; nprev = e->state.n; }
        else if (e->tail) { tailv[0] = e->tail; prev = tailv; nprev = 1; }
        else { prev = NULL; nprev = 0; }
    }
    translation_t **dl = NULL;
    int nd = ndo + (extra ? 1 : 0);
    if (nd) {
        dl = malloc((size_t)nd * sizeof(*dl));
        if (!dl) abort();
        for (int i = 0; i < ndo; i++) dl[i] = dolist[i];
        if (extra) dl[ndo] = extra;
    }
    trlist_t undo = e->to_undo;
    memset(&e->to_undo, 0, sizeof(e->to_undo));
    e->to_do = 0;
    if (undo.n || nd) format_translations(e, undo.p, undo.n, dl, nd, prev, nprev);
    for (int i = 0; i < undo.n; i++) tr_release(undo.p[i]);
    free(undo.p);
    free(dl);
    resize_translations(e);
}

/* ---- lookups ---- */
typedef struct { steno_stroke_t key; char *mapping; } suffix_t;

static int involved_suffixes(steno_engine_t *e, steno_stroke_t stroke, suffix_t *out)
{
    int n = 0;
    for (int i = 0; i < 4; i++) {
        steno_stroke_t suf = STENO_MASK(SUFFIX_KEYS[i]);
        if ((stroke & suf) != suf) continue;
        char *m = steno_dict_lookup(e->dict, &suf, 1);
        if (!m) continue;
        out[n].key = suf;
        out[n].mapping = m;
        n++;
    }
    return n;
}

static char *lookup_with_suffix(steno_engine_t *e, const steno_stroke_t *strokes, int n, const suffix_t *suf, int nsuf)
{
    if (nsuf == 0) return steno_dict_lookup(e->dict, strokes, n);
    steno_stroke_t tmp[STENO_MAX_STROKES];
    for (int i = 0; i < nsuf; i++) {
        memcpy(tmp, strokes, (size_t)n * sizeof(*tmp));
        tmp[n - 1] &= ~suf[i].key;
        char *main = steno_dict_lookup(e->dict, tmp, n);
        if (main) {
            sbuf_t b;
            sb_init(&b);
            sb_adds(&b, main);
            sb_addc(&b, ' ');
            sb_adds(&b, suf[i].mapping);
            free(main);
            return sb_take(&b);
        }
    }
    return NULL;
}

static translation_t *find_longest_match(steno_engine_t *e, int min_len, steno_stroke_t stroke, bool use_suffix)
{
    suffix_t suf[4];
    int nsuf = 0;
    if (use_suffix) {
        nsuf = involved_suffixes(e, stroke, suf);
        if (!nsuf) return NULL;
    }
    int num_strokes = 1, count = 0;
    for (int i = e->state.n - 1; i >= 0; i--) {
        num_strokes += e->state.p[i]->n;
        if (num_strokes > e->max_len) break;
        count++;
    }
    int base = e->state.n - count;
    translation_t *result = NULL;
    for (int i = 0; i <= count && !result; i++) {
        steno_stroke_t strokes[STENO_MAX_STROKES + 1];
        int n = 0;
        for (int k = base + i; k < e->state.n; k++)
            for (int j = 0; j < e->state.p[k]->n; j++) strokes[n++] = e->state.p[k]->strokes[j];
        strokes[n++] = stroke;
        if (n < min_len || n > e->max_len) continue;
        char *mapping = lookup_with_suffix(e, strokes, n, suf, nsuf);
        if (mapping) {
            result = tr_new(strokes, n, mapping);
            int nr = e->state.n - (base + i);
            if (nr) {
                result->replaced = malloc((size_t)nr * sizeof(*result->replaced));
                if (!result->replaced) abort();
                for (int k = 0; k < nr; k++) result->replaced[k] = tr_ref(e->state.p[base + i + k]);
                result->nreplaced = nr;
            }
        }
    }
    for (int i = 0; i < nsuf; i++) free(suf[i].mapping);
    return result;
}

/* ---- macros ---- */
static void translate_stroke(steno_engine_t *e, steno_stroke_t stroke);

static char *translator_lookup(steno_engine_t *e, steno_stroke_t stroke)
{
    char *r = steno_dict_lookup(e->dict, &stroke, 1);
    if (r) return r;
    suffix_t suf[4];
    int n = involved_suffixes(e, stroke, suf);
    if (!n) return NULL;
    r = lookup_with_suffix(e, &stroke, 1, suf, n);
    for (int i = 0; i < n; i++) free(suf[i].mapping);
    return r;
}

static void macro_undo(steno_engine_t *e, steno_stroke_t stroke)
{
    while (e->state.n > 0) {
        translation_t *t = tr_ref(e->state.p[e->state.n - 1]);
        untranslate_translation(e, t);
        bool has = tr_has_undo(t);
        tr_release(t);
        if (has) return;
    }
    translation_t *extra = tr_new(&stroke, 1, sx_dup("{#Control_L(BackSpace)}{^}"));
    flush(e, extra);
    tr_release(extra);
}

static void macro_repeat(steno_engine_t *e)
{
    if (!e->state.n) return;
    translation_t *t = e->state.p[e->state.n - 1];
    translate_stroke(e, t->strokes[t->n - 1]);
}

static void macro_toggle_asterisk(steno_engine_t *e)
{
    if (!e->state.n) return;
    translation_t *t = tr_ref(e->state.p[e->state.n - 1]);
    untranslate_translation(e, t);
    steno_stroke_t s = t->strokes[t->n - 1] ^ STENO_MASK(STENO_KEY_STAR);
    tr_release(t);
    translate_stroke(e, s);
}

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

static void macro_delete_space(steno_engine_t *e, steno_stroke_t stroke)
{
    if (e->state.n < 2) return;
    translation_t *r0 = e->state.p[e->state.n - 2], *r1 = e->state.p[e->state.n - 1];
    if (r1->is_retro) return;
    sbuf_t b;
    sb_init(&b);
    int count = 0;
    translation_t *rs[2] = { r0, r1 };
    for (int i = 0; i < 2; i++) {
        translation_t *t = rs[i];
        const char *piece = NULL;
        char tmp[48];
        if (t->english) piece = t->english;
        else if (t->n == 1) {
            char s[40];
            steno_stroke_to_str(t->strokes[0], s);
            if (all_digits(s)) { snprintf(tmp, sizeof(tmp), "{&%s}", s); piece = tmp; }
        }
        if (piece) {
            if (count) sb_adds(&b, "{^~|^}");
            sb_adds(&b, piece);
            count++;
        }
    }
    if (count > 1) {
        translation_t *t = tr_new(&stroke, 1, sb_take(&b));
        t->replaced = malloc(2 * sizeof(*t->replaced));
        if (!t->replaced) abort();
        t->replaced[0] = tr_ref(r0);
        t->replaced[1] = tr_ref(r1);
        t->nreplaced = 2;
        t->is_retro = true;
        translate_translation(e, t);
        tr_release(t);
    } else sb_free(&b);
}

static void macro_insert_space(steno_engine_t *e, steno_stroke_t stroke)
{
    if (!e->state.n) return;
    translation_t *rep = e->state.p[e->state.n - 1];
    if (rep->is_retro) return;
    if (rep->nreplaced == 0) return;
    steno_stroke_t ls = rep->strokes[rep->n - 1];
    sbuf_t b;
    sb_init(&b);
    for (int i = 0; i < rep->nreplaced; i++) {
        translation_t *t = rep->replaced[i];
        if (i) sb_addc(&b, ' ');
        if (t->english && t->english[0]) sb_adds(&b, t->english);
        else {
            for (int j = 0; j < t->n; j++) {
                char s[40];
                if (j) sb_addc(&b, '/');
                steno_stroke_to_str(t->strokes[j], s);
                sb_adds(&b, s);
            }
        }
    }
    char *lk = translator_lookup(e, ls);
    sb_addc(&b, ' ');
    if (lk) { sb_adds(&b, lk); free(lk); }
    else { char s[40]; steno_stroke_to_str(ls, s); sb_adds(&b, s); }
    translation_t *t = tr_new(&stroke, 1, sb_take(&b));
    t->replaced = malloc(sizeof(*t->replaced));
    if (!t->replaced) abort();
    t->replaced[0] = tr_ref(rep);
    t->nreplaced = 1;
    t->is_retro = true;
    translate_translation(e, t);
    tr_release(t);
}

/* Returns 1 if `mapping` is a macro (and runs it) */
static bool try_macro(steno_engine_t *e, const char *mapping, steno_stroke_t stroke)
{
    const char *name = NULL;
    char buf[40];
    if (!strcmp(mapping, "{*}")) name = "retro_toggle_asterisk";
    else if (!strcmp(mapping, "{*!}")) name = "retro_delete_space";
    else if (!strcmp(mapping, "{*?}")) name = "retro_insert_space";
    else if (!strcmp(mapping, "{*+}")) name = "repeat_last_stroke";
    else if (mapping[0] == '=') {
        size_t i = 1;
        while (sx_isword((unsigned char)mapping[i])) i++;
        if (i == 1 || (mapping[i] != 0 && mapping[i] != ':')) return false;
        if (i >= sizeof(buf)) return true;
        memcpy(buf, mapping + 1, i - 1);
        buf[i - 1] = 0;
        name = buf;
    } else return false;

    if (!strcmp(name, "undo")) macro_undo(e, stroke);
    else if (!strcmp(name, "repeat_last_stroke")) macro_repeat(e);
    else if (!strcmp(name, "retro_toggle_asterisk") || !strcmp(name, "retrospective_toggle_asterisk")) macro_toggle_asterisk(e);
    else if (!strcmp(name, "retro_delete_space") || !strcmp(name, "retrospective_delete_space")) macro_delete_space(e, stroke);
    else if (!strcmp(name, "retro_insert_space") || !strcmp(name, "retrospective_insert_space")) macro_insert_space(e, stroke);
    return true;
}

static void translate_stroke(steno_engine_t *e, steno_stroke_t stroke)
{
    char *mapping = steno_dict_lookup(e->dict, &stroke, 1);
    if (!mapping && stroke == STENO_MASK(STENO_KEY_STAR)) { macro_undo(e, stroke); return; }
    if (mapping && try_macro(e, mapping, stroke)) { free(mapping); return; }

    translation_t *t = find_longest_match(e, 2, stroke, false);
    if (!t && mapping) { t = tr_new(&stroke, 1, mapping); mapping = NULL; }
    if (!t) t = find_longest_match(e, 1, stroke, true);
    if (!t) t = tr_new(&stroke, 1, NULL);
    free(mapping);
    translate_translation(e, t);
    tr_release(t);
}

/* ================================================================== public */
steno_engine_t *steno_engine_new(steno_dict_t *dict, const steno_output_t *out, int undo_levels)
{
    steno_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    steno_ortho_init();
    e->dict = dict;
    e->out = *out;
    e->undo_levels = undo_levels < 1 ? 1 : undo_levels;
    e->max_len = steno_dict_longest_key(dict);
    e->output_enabled = true;
    return e;
}

void steno_engine_free(steno_engine_t *e)
{
    if (!e) return;
    for (int i = 0; i < e->state.n; i++) tr_release(e->state.p[i]);
    free(e->state.p);
    for (int i = 0; i < e->to_undo.n; i++) tr_release(e->to_undo.p[i]);
    free(e->to_undo.p);
    tr_release(e->tail);
    free(e);
}

void steno_engine_stroke(steno_engine_t *e, steno_stroke_t stroke)
{
    translate_stroke(e, stroke);
    flush(e, NULL);
}

bool steno_engine_output_enabled(const steno_engine_t *e) { return e->output_enabled; }
void steno_engine_set_output_enabled(steno_engine_t *e, bool enabled)
{
    if (e->output_enabled != enabled) {
        e->output_enabled = enabled;
        if (e->out.on_output_enabled) e->out.on_output_enabled(e->out.ctx, enabled);
    }
}
