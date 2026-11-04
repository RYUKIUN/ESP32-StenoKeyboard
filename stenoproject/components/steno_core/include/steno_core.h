/*
 * steno_core - portable (no ESP-IDF dependency) Plover-compatible steno engine.
 *
 *   strokes (23-bit masks)  ->  dictionary lookup  ->  Plover translator/formatter
 *                           ->  output events (backspaces / text / key combos)
 *
 * The same sources are compiled on the PC by tools/hosttest to compare against real Plover.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- strokes ---------------------------------------------------------- */
typedef uint32_t steno_stroke_t;

/* Bit index of each steno key inside a stroke mask (Plover English Stenotype order). */
enum steno_key {
    STENO_KEY_NUM = 0,
    STENO_KEY_S_L, STENO_KEY_T_L, STENO_KEY_K_L, STENO_KEY_P_L, STENO_KEY_W_L,
    STENO_KEY_H_L, STENO_KEY_R_L, STENO_KEY_A_L, STENO_KEY_O_L,
    STENO_KEY_STAR,
    STENO_KEY_E_R, STENO_KEY_U_R,
    STENO_KEY_F_R, STENO_KEY_R_R, STENO_KEY_P_R, STENO_KEY_B_R, STENO_KEY_L_R,
    STENO_KEY_G_R, STENO_KEY_T_R, STENO_KEY_S_R, STENO_KEY_D_R, STENO_KEY_Z_R,
    STENO_KEY_COUNT /* 23 */
};
#define STENO_KEY_NONE 0xFF
#define STENO_MASK(key) (((steno_stroke_t)1) << (key))
#define STENO_MAX_STROKES 12

/* Mask -> RTFCRE text ("KAT", "-T", "1-6" ...). `out` needs >= 32 bytes. */
void steno_stroke_to_str(steno_stroke_t s, char *out);

/* ---- dictionary blob -------------------------------------------------- */
/* read() must return a pointer to `len` readable bytes at `off`. The pointer must stay valid
 * until the next call to read() (memory-mapped flash: always valid). */
typedef struct {
    const uint8_t *(*read)(void *ctx, uint32_t off, uint32_t len);
    void *ctx;
    uint32_t size;
} steno_blob_t;

typedef struct steno_dict steno_dict_t;

steno_dict_t *steno_dict_open(const steno_blob_t *blob);   /* NULL if header invalid */
void steno_dict_close(steno_dict_t *d);
int steno_dict_longest_key(const steno_dict_t *d);
uint32_t steno_dict_entry_count(const steno_dict_t *d);
/* Returns a malloc()ed string or NULL. */
char *steno_dict_lookup(steno_dict_t *d, const steno_stroke_t *strokes, int n);
/* Orthography wordlist: -1 if absent, otherwise weight class 0..8 (lower = more common). */
int steno_dict_word_class(steno_dict_t *d, const char *word, size_t len);

/* ---- engine ----------------------------------------------------------- */
typedef struct {
    void (*send_backspaces)(void *ctx, int count);
    void (*send_string)(void *ctx, const char *text, size_t len);
    void (*send_combo)(void *ctx, const char *combo);        /* Plover syntax: "Control_L(c)" */
    void (*on_output_enabled)(void *ctx, bool enabled);      /* optional: steno suspended/resumed */
    void *ctx;
} steno_output_t;

typedef struct steno_engine steno_engine_t;

steno_engine_t *steno_engine_new(steno_dict_t *dict, const steno_output_t *out, int undo_levels);
void steno_engine_free(steno_engine_t *e);
/* Feed one finished stroke (all keys released). */
void steno_engine_stroke(steno_engine_t *e, steno_stroke_t stroke);
bool steno_engine_output_enabled(const steno_engine_t *e);
void steno_engine_set_output_enabled(steno_engine_t *e, bool enabled);

#ifdef __cplusplus
}
#endif
