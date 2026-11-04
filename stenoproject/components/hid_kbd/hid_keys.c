#include "hid_keys.h"

#include <string.h>

typedef struct { const char *name; uint8_t code; } keyname_t;

static const keyname_t NAMES[] = {
    { "space", 0x2C }, { "Return", 0x28 }, { "Escape", 0x29 }, { "BackSpace", 0x2A }, { "Tab", 0x2B },
    { "minus", 0x2D }, { "equal", 0x2E }, { "bracketleft", 0x2F }, { "bracketright", 0x30 },
    { "backslash", 0x31 }, { "semicolon", 0x33 }, { "apostrophe", 0x34 }, { "grave", 0x35 },
    { "comma", 0x36 }, { "period", 0x37 }, { "slash", 0x38 }, { "Caps_Lock", 0x39 },
    { "F1", 0x3A }, { "F2", 0x3B }, { "F3", 0x3C }, { "F4", 0x3D }, { "F5", 0x3E }, { "F6", 0x3F },
    { "F7", 0x40 }, { "F8", 0x41 }, { "F9", 0x42 }, { "F10", 0x43 }, { "F11", 0x44 }, { "F12", 0x45 },
    { "Print", 0x46 }, { "Scroll_Lock", 0x47 }, { "Pause", 0x48 }, { "Insert", 0x49 }, { "Home", 0x4A },
    { "Page_Up", 0x4B }, { "Prior", 0x4B }, { "Delete", 0x4C }, { "End", 0x4D }, { "Page_Down", 0x4E },
    { "Next", 0x4E }, { "Right", 0x4F }, { "Left", 0x50 }, { "Down", 0x51 }, { "Up", 0x52 },
    { "Num_Lock", 0x53 }, { "Menu", 0x65 }, { "KP_Enter", 0x58 },
    { "F13", 0x68 }, { "F14", 0x69 }, { "F15", 0x6A }, { "F16", 0x6B }, { "F17", 0x6C }, { "F18", 0x6D },
    { "F19", 0x6E }, { "F20", 0x6F }, { "F21", 0x70 }, { "F22", 0x71 }, { "F23", 0x72 }, { "F24", 0x73 },
};
static const keyname_t MODS[] = {
    { "Control_L", 0x01 }, { "Shift_L", 0x02 }, { "Alt_L", 0x04 }, { "Super_L", 0x08 },
    { "Control_R", 0x10 }, { "Shift_R", 0x20 }, { "Alt_R", 0x40 }, { "Super_R", 0x80 },
    { "Meta_L", 0x04 }, { "Meta_R", 0x40 },
};

bool hid_ascii(char c, uint8_t *mods, uint8_t *usage)
{
    uint8_t m = 0, u = 0;
    if (c >= 'a' && c <= 'z') u = (uint8_t)(0x04 + c - 'a');
    else if (c >= 'A' && c <= 'Z') { u = (uint8_t)(0x04 + c - 'A'); m = 0x02; }
    else if (c >= '1' && c <= '9') u = (uint8_t)(0x1E + c - '1');
    else if (c == '0') u = 0x27;
    else {
        static const char *shifted = "!@#$%^&*()";   /* 1..0 */
        const char *p = c ? strchr(shifted, c) : NULL;
        if (p) { u = (uint8_t)(0x1E + (p - shifted)); m = 0x02; }
        else switch (c) {
        case '\n': case '\r': u = 0x28; break;
        case '\t': u = 0x2B; break;
        case ' ': u = 0x2C; break;
        case '-': u = 0x2D; break;
        case '_': u = 0x2D; m = 2; break;
        case '=': u = 0x2E; break;
        case '+': u = 0x2E; m = 2; break;
        case '[': u = 0x2F; break;
        case '{': u = 0x2F; m = 2; break;
        case ']': u = 0x30; break;
        case '}': u = 0x30; m = 2; break;
        case '\\': u = 0x31; break;
        case '|': u = 0x31; m = 2; break;
        case ';': u = 0x33; break;
        case ':': u = 0x33; m = 2; break;
        case '\'': u = 0x34; break;
        case '"': u = 0x34; m = 2; break;
        case '`': u = 0x35; break;
        case '~': u = 0x35; m = 2; break;
        case ',': u = 0x36; break;
        case '<': u = 0x36; m = 2; break;
        case '.': u = 0x37; break;
        case '>': u = 0x37; m = 2; break;
        case '/': u = 0x38; break;
        case '?': u = 0x38; m = 2; break;
        default: return false;
        }
    }
    *mods = m;
    *usage = u;
    return true;
}

static size_t put(hid_report_t *out, size_t n, size_t max, uint8_t mods, const uint8_t *keys, int nkeys)
{
    if (n >= max) return n;
    memset(&out[n], 0, sizeof(out[n]));
    out[n].mods = mods;
    for (int i = 0; i < nkeys && i < 6; i++) out[n].keys[i] = keys[i];
    return n + 1;
}

size_t hid_text_to_reports(const char *text, size_t len, hid_report_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t m, u;
        if (!hid_ascii(text[i], &m, &u)) continue;
        if (n + 2 > max) break;
        n = put(out, n, max, m, &u, 1);
        n = put(out, n, max, 0, NULL, 0);
    }
    return n;
}

/* ---- combo parser ---- */
typedef struct {
    const char *p;
    hid_report_t *out;
    size_t n, max;
    uint8_t mods;
    uint8_t keys[6];
    int nkeys;
    int err;
    int depth;
} cp_t;

static void emit(cp_t *c) { c->n = put(c->out, c->n, c->max, c->mods, c->keys, c->nkeys); }

static bool is_name_char(char ch)
{
    return ch && ch != '(' && ch != ')' && ch != ' ' && ch != '\t';
}

static void parse_seq(cp_t *c)
{
    if (++c->depth > 8) { c->err = 1; return; }
    for (;;) {
        while (*c->p == ' ' || *c->p == '\t') c->p++;
        if (!*c->p || *c->p == ')') break;
        const char *s = c->p;
        while (is_name_char(*c->p)) c->p++;
        size_t len = (size_t)(c->p - s);
        if (!len) { c->err = 1; return; }

        uint8_t addmods = 0, key = 0;
        for (size_t i = 0; i < sizeof(MODS) / sizeof(MODS[0]); i++)
            if (strlen(MODS[i].name) == len && !strncmp(MODS[i].name, s, len)) addmods = MODS[i].code;
        if (!addmods) {
            for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
                if (strlen(NAMES[i].name) == len && !strncmp(NAMES[i].name, s, len)) key = NAMES[i].code;
            if (!key && len == 1) {
                uint8_t m;
                if (hid_ascii(s[0], &m, &key)) addmods = m;
            }
            if (!key) { c->err = 1; return; }
        }
        uint8_t saved_mods = c->mods;
        int saved_nkeys = c->nkeys;
        c->mods |= addmods;
        if (key) {
            if (c->nkeys >= 6) { c->err = 1; return; }
            c->keys[c->nkeys++] = key;
        }
        emit(c);
        if (*c->p == '(') {
            c->p++;
            parse_seq(c);
            if (c->err) return;
            if (*c->p != ')') { c->err = 1; return; }
            c->p++;
        }
        c->mods = saved_mods;
        c->nkeys = saved_nkeys;
        if (key) c->mods = saved_mods;
        emit(c);
    }
    c->depth--;
}

int hid_combo_to_reports(const char *combo, hid_report_t *out, size_t max)
{
    cp_t c;
    memset(&c, 0, sizeof(c));
    c.p = combo;
    c.out = out;
    c.max = max;
    parse_seq(&c);
    if (c.err || *c.p) return -1;
    return (int)c.n;
}
