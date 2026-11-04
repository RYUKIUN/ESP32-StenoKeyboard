/*
 * regex-lite: just enough of Python's `re` for Plover's orthography rules.
 * Supports: literals, '.', [classes], (groups), (?:groups), alternation, * + ? on single
 * atoms, ^ $, escapes, and fixed-length negative look-behind (?<!...). Always IGNORECASE.
 */
#include <stdlib.h>
#include <string.h>

#include "steno_internal.h"

enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_BOL, OP_EOL, OP_LBNEG, OP_LBEND, OP_MATCH };

typedef struct { uint8_t op; int a, b; } inst_t;
typedef struct { uint8_t bits[32]; } cls_t;

struct re_prog {
    inst_t *code;
    int ncode, capcode;
    cls_t *cls;
    int ncls, capcls;
    int ngroups;
};

/* ------------------------------------------------------------- AST */
enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_GROUP, N_ALT, N_SEQ, N_STAR, N_PLUS, N_QUEST, N_LBNEG };
typedef struct node {
    int type, val;              /* char / class index / group index (-1 = non capturing) */
    struct node *kid;           /* child (group, quantifier, lookbehind) */
    struct node *next;          /* sibling in seq / alt list */
} node_t;

typedef struct { const char *s; int pos; re_prog_t *prog; int err; int groups; } parser_t;

static node_t *nn(int type)
{
    node_t *n = calloc(1, sizeof(*n));
    if (!n) abort();
    n->type = type;
    return n;
}
static void free_node(node_t *n)
{
    while (n) {
        node_t *nx = n->next;
        free_node(n->kid);
        free(n);
        n = nx;
    }
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void cls_set(cls_t *c, int ch)
{
    c->bits[ch >> 3] |= (uint8_t)(1 << (ch & 7));
    if (ch >= 'a' && ch <= 'z') { int u = ch - 32; c->bits[u >> 3] |= (uint8_t)(1 << (u & 7)); }
    if (ch >= 'A' && ch <= 'Z') { int l = ch + 32; c->bits[l >> 3] |= (uint8_t)(1 << (l & 7)); }
}

static int add_class(parser_t *P, const cls_t *c)
{
    re_prog_t *p = P->prog;
    if (p->ncls == p->capcls) {
        p->capcls = p->capcls ? p->capcls * 2 : 8;
        p->cls = realloc(p->cls, (size_t)p->capcls * sizeof(cls_t));
        if (!p->cls) abort();
    }
    p->cls[p->ncls] = *c;
    return p->ncls++;
}

static node_t *parse_alt(parser_t *P);

static node_t *parse_class(parser_t *P)
{
    cls_t c;
    memset(&c, 0, sizeof(c));
    bool neg = false;
    if (P->s[P->pos] == '^') { neg = true; P->pos++; }
    bool first = true;
    while (P->s[P->pos] && (P->s[P->pos] != ']' || first)) {
        first = false;
        int ch = (unsigned char)P->s[P->pos++];
        if (ch == '\\') ch = (unsigned char)P->s[P->pos++];
        if (P->s[P->pos] == '-' && P->s[P->pos + 1] && P->s[P->pos + 1] != ']') {
            int hi = (unsigned char)P->s[P->pos + 1];
            P->pos += 2;
            for (int x = ch; x <= hi; x++) cls_set(&c, x);
        } else cls_set(&c, ch);
    }
    if (P->s[P->pos] != ']') { P->err = 1; return NULL; }
    P->pos++;
    if (neg) for (int i = 0; i < 32; i++) c.bits[i] = (uint8_t)~c.bits[i];
    node_t *n = nn(N_CLASS);
    n->val = add_class(P, &c);
    return n;
}

static node_t *parse_atom(parser_t *P)
{
    char ch = P->s[P->pos];
    node_t *n;
    if (ch == '(') {
        P->pos++;
        int cap = -1;
        bool lb = false;
        if (P->s[P->pos] == '?') {
            if (P->s[P->pos + 1] == ':') P->pos += 2;
            else if (P->s[P->pos + 1] == '<' && P->s[P->pos + 2] == '!') { P->pos += 3; lb = true; }
            else { P->err = 1; return NULL; }
        } else cap = ++P->groups;
        node_t *inner = parse_alt(P);
        if (P->err || P->s[P->pos] != ')') { P->err = 1; free_node(inner); return NULL; }
        P->pos++;
        n = nn(lb ? N_LBNEG : N_GROUP);
        n->val = cap;
        n->kid = inner;
        return n;
    }
    if (ch == '[') { P->pos++; return parse_class(P); }
    if (ch == '.') { P->pos++; return nn(N_ANY); }
    if (ch == '^') { P->pos++; return nn(N_BOL); }
    if (ch == '$') { P->pos++; return nn(N_EOL); }
    if (ch == '\\') {
        P->pos++;
        n = nn(N_CHAR);
        n->val = lc((unsigned char)P->s[P->pos++]);
        return n;
    }
    P->pos++;
    n = nn(N_CHAR);
    n->val = lc((unsigned char)ch);
    return n;
}

static node_t *parse_seq(parser_t *P)
{
    node_t *head = nn(N_SEQ), **tail = &head->kid;
    while (!P->err && P->s[P->pos] && P->s[P->pos] != '|' && P->s[P->pos] != ')') {
        node_t *a = parse_atom(P);
        if (!a) break;
        char q = P->s[P->pos];
        if (q == '*' || q == '+' || q == '?') {
            P->pos++;
            node_t *qn = nn(q == '*' ? N_STAR : (q == '+' ? N_PLUS : N_QUEST));
            qn->kid = a;
            a = qn;
        }
        *tail = a;
        tail = &a->next;
    }
    return head;
}

static node_t *parse_alt(parser_t *P)
{
    node_t *alt = nn(N_ALT), **tail = &alt->kid;
    for (;;) {
        node_t *s = parse_seq(P);
        *tail = s;
        tail = &s->next;
        if (P->s[P->pos] == '|') P->pos++;
        else break;
    }
    return alt;
}

/* ------------------------------------------------------------ codegen */
static int emit(re_prog_t *p, int op, int a, int b)
{
    if (p->ncode == p->capcode) {
        p->capcode = p->capcode ? p->capcode * 2 : 64;
        p->code = realloc(p->code, (size_t)p->capcode * sizeof(inst_t));
        if (!p->code) abort();
    }
    p->code[p->ncode] = (inst_t){ (uint8_t)op, a, b };
    return p->ncode++;
}

static int fixed_len(const node_t *n)
{
    switch (n->type) {
    case N_CHAR: case N_ANY: case N_CLASS: return 1;
    case N_BOL: case N_EOL: return 0;
    case N_SEQ: { int t = 0; for (const node_t *k = n->kid; k; k = k->next) { int l = fixed_len(k); if (l < 0) return -1; t += l; } return t; }
    case N_ALT: { int l0 = -2; for (const node_t *k = n->kid; k; k = k->next) { int l = fixed_len(k); if (l < 0 || (l0 != -2 && l != l0)) return -1; l0 = l; } return l0 < 0 ? 0 : l0; }
    case N_GROUP: return fixed_len(n->kid);
    default: return -1;
    }
}

static void gen(re_prog_t *p, const node_t *n)
{
    switch (n->type) {
    case N_CHAR: emit(p, OP_CHAR, n->val, 0); break;
    case N_ANY: emit(p, OP_ANY, 0, 0); break;
    case N_CLASS: emit(p, OP_CLASS, n->val, 0); break;
    case N_BOL: emit(p, OP_BOL, 0, 0); break;
    case N_EOL: emit(p, OP_EOL, 0, 0); break;
    case N_SEQ: for (const node_t *k = n->kid; k; k = k->next) gen(p, k); break;
    case N_GROUP:
        if (n->val >= 0) emit(p, OP_SAVE, 2 * n->val, 0);
        gen(p, n->kid);
        if (n->val >= 0) emit(p, OP_SAVE, 2 * n->val + 1, 0);
        break;
    case N_ALT: {
        int jmps[32], nj = 0;
        const node_t *k = n->kid;
        while (k) {
            if (k->next) {
                int sp = emit(p, OP_SPLIT, 0, 0);
                p->code[sp].a = p->ncode;
                gen(p, k);
                jmps[nj++] = emit(p, OP_JMP, 0, 0);
                p->code[sp].b = p->ncode;
            } else gen(p, k);
            k = k->next;
        }
        for (int i = 0; i < nj; i++) p->code[jmps[i]].a = p->ncode;
        break;
    }
    case N_STAR: {
        int sp = emit(p, OP_SPLIT, 0, 0);
        p->code[sp].a = p->ncode;
        gen(p, n->kid);
        emit(p, OP_JMP, sp, 0);
        p->code[sp].b = p->ncode;
        break;
    }
    case N_PLUS: {
        int start = p->ncode;
        gen(p, n->kid);
        int sp = emit(p, OP_SPLIT, start, 0);
        p->code[sp].b = p->ncode;
        break;
    }
    case N_QUEST: {
        int sp = emit(p, OP_SPLIT, 0, 0);
        p->code[sp].a = p->ncode;
        gen(p, n->kid);
        p->code[sp].b = p->ncode;
        break;
    }
    case N_LBNEG: {
        int len = fixed_len(n->kid);
        int lb = emit(p, OP_LBNEG, len < 0 ? 0 : len, 0);
        gen(p, n->kid);
        emit(p, OP_LBEND, 0, 0);
        p->code[lb].b = p->ncode;   /* continuation */
        break;
    }
    }
}

re_prog_t *re_compile(const char *pattern)
{
    re_prog_t *prog = calloc(1, sizeof(*prog));
    if (!prog) abort();
    parser_t P = { pattern, 0, prog, 0, 0 };
    node_t *ast = parse_alt(&P);
    if (P.err || pattern[P.pos]) { free_node(ast); re_free(prog); return NULL; }
    prog->ngroups = P.groups;
    emit(prog, OP_SAVE, 0, 0);
    gen(prog, ast);
    emit(prog, OP_SAVE, 1, 0);
    emit(prog, OP_MATCH, 0, 0);
    free_node(ast);
    return prog;
}

void re_free(re_prog_t *p)
{
    if (!p) return;
    free(p->code);
    free(p->cls);
    free(p);
}

/* ------------------------------------------------------------- matcher */
typedef struct {
    const re_prog_t *p;
    const char *s;
    int len;
    int caps[22];
    int lb_target;
} mctx_t;

static bool run(mctx_t *m, int pc, int sp)
{
    const inst_t *code = m->p->code;
    for (;;) {
        const inst_t *i = &code[pc];
        switch (i->op) {
        case OP_CHAR:
            if (sp >= m->len || lc((unsigned char)m->s[sp]) != i->a) return false;
            sp++; pc++; break;
        case OP_ANY:
            if (sp >= m->len || m->s[sp] == '\n') return false;
            sp++; pc++; break;
        case OP_CLASS: {
            if (sp >= m->len) return false;
            int ch = (unsigned char)m->s[sp];
            if (!(m->p->cls[i->a].bits[ch >> 3] >> (ch & 7) & 1)) return false;
            sp++; pc++; break;
        }
        case OP_BOL: if (sp != 0) return false; pc++; break;
        case OP_EOL: if (sp != m->len) return false; pc++; break;
        case OP_JMP: pc = i->a; break;
        case OP_SPLIT: {
            int saved[22];
            memcpy(saved, m->caps, sizeof(saved));
            if (run(m, i->a, sp)) return true;
            memcpy(m->caps, saved, sizeof(saved));
            pc = i->b;
            break;
        }
        case OP_SAVE: m->caps[i->a] = sp; pc++; break;
        case OP_LBNEG: {
            bool matched = false;
            if (sp >= i->a) {
                int old = m->lb_target;
                int saved[22];
                memcpy(saved, m->caps, sizeof(saved));
                m->lb_target = sp;
                matched = run(m, pc + 1, sp - i->a);
                m->lb_target = old;
                memcpy(m->caps, saved, sizeof(saved));
            }
            if (matched) return false;
            pc = i->b;
            break;
        }
        case OP_LBEND: return sp == m->lb_target;
        case OP_MATCH: return true;
        default: return false;
        }
    }
}

bool re_match(const re_prog_t *p, const char *s, int len, int *caps)
{
    mctx_t m;
    m.p = p; m.s = s; m.len = len; m.lb_target = -1;
    for (int i = 0; i < 22; i++) m.caps[i] = -1;
    if (!run(&m, 0, 0)) return false;
    if (caps) memcpy(caps, m.caps, sizeof(m.caps));
    return true;
}
