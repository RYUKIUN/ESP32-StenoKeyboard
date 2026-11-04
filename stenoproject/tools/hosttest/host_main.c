/* usage: host_main dict.bin < strokes.txt  (one hex stroke mask per line; blank line = reset)
 * prints the resulting text; combos shown as <combo>. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "steno_core.h"

static uint8_t *blob; static uint32_t blen;
static const uint8_t *rd(void *c, uint32_t off, uint32_t len) { (void)c; (void)len; return blob + off; }
static char buf[1 << 20]; static size_t bl;
static void bs(void *c, int n) { (void)c; while (n-- && bl) bl--; }
static void ss(void *c, const char *t, size_t l) { (void)c; memcpy(buf + bl, t, l); bl += l; }
static void cb(void *c, const char *k) { (void)c; bl += sprintf(buf + bl, "<%s>", k); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); blen = ftell(f); rewind(f);
    blob = malloc(blen); if (fread(blob, 1, blen, f) != blen) return 2; fclose(f);
    steno_blob_t b = { rd, NULL, blen };
    steno_dict_t *d = steno_dict_open(&b);
    if (!d) { fprintf(stderr, "bad dict\n"); return 2; }
    steno_output_t o = { bs, ss, cb, NULL, NULL };
    steno_engine_t *e = steno_engine_new(d, &o, 100);
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == '\n') break;
        steno_engine_stroke(e, (steno_stroke_t)strtoul(line, NULL, 16));
    }
    buf[bl] = 0; fputs(buf, stdout);
    steno_engine_free(e); steno_dict_close(d); free(blob);
    return 0;
}
