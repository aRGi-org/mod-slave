// =============================================================================
//  jt_helper.c - Implementazione helper "cJSON-like" sopra jsmn
// =============================================================================
#define JSMN_STATIC
#include "jsmn.h"
#include "jt_helper.h"
#include <string.h>
#include <stdlib.h>

// I token sono jsmntok_t; li trattiamo via void* nell'API per non esporre jsmn
// negli header dei chiamanti. Qui castiamo.
#define TOKS(p) ((const jsmntok_t *)(p))

int jt_parse(const char *js, size_t len, void *tokens, unsigned int max_tokens)
{
    jsmn_parser p;
    jsmn_init(&p);
    return jsmn_parse(&p, js, len, (jsmntok_t *)tokens, max_tokens);
}

bool jt_streq(const char *js, const void *tokens, int tok_idx, const char *s)
{
    const jsmntok_t *t = &TOKS(tokens)[tok_idx];
    if (t->type != JSMN_STRING && t->type != JSMN_PRIMITIVE) return false;
    int len = t->end - t->start;
    return ((int)strlen(s) == len) && (strncmp(js + t->start, s, len) == 0);
}

// Salta il token 'i' e TUTTI i suoi discendenti, ritornando l'indice del token
// successivo allo stesso livello. Necessario per navigare oggetti/array annidati
// senza un albero: per oggetti e array, i figli seguono in sequenza nell'array
// di token, e 'size' indica quanti figli diretti ha il nodo.
static int skip(const jsmntok_t *t, int i)
{
    int children = t[i].size;
    int j = i + 1;
    if (t[i].type == JSMN_OBJECT) {
        // Ogni figlio di un oggetto e' una coppia chiave(1) + valore(sottoalbero).
        for (int c = 0; c < children; c++) {
            j++;              // salta la chiave
            j = skip(t, j);   // salta il valore (ricorsivo)
        }
    } else if (t[i].type == JSMN_ARRAY) {
        for (int c = 0; c < children; c++) {
            j = skip(t, j);   // salta ogni elemento (ricorsivo)
        }
    }
    // primitivi e stringhe non hanno figli: j = i+1 va bene
    return j;
}

int jt_get(const char *js, const void *tokens, int ntok, int obj_idx, const char *key)
{
    const jsmntok_t *t = TOKS(tokens);
    if (obj_idx < 0 || obj_idx >= ntok) return -1;
    if (t[obj_idx].type != JSMN_OBJECT) return -1;
    int children = t[obj_idx].size;
    int j = obj_idx + 1;
    for (int c = 0; c < children; c++) {
        // j punta alla chiave; il valore e' j+1
        if (jt_streq(js, tokens, j, key)) {
            return j + 1;
        }
        j++;              // salta la chiave
        j = skip(t, j);   // salta il valore
    }
    return -1;
}

int jt_str_tok(const char *js, const void *tokens, int val_idx, char *out, size_t out_sz)
{
    if (out_sz == 0) return 0;
    if (val_idx < 0) { out[0] = '\0'; return 0; }
    const jsmntok_t *t = &TOKS(tokens)[val_idx];
    int len = t->end - t->start;
    if (len < 0) len = 0;
    if ((size_t)len >= out_sz) len = (int)out_sz - 1;
    memcpy(out, js + t->start, len);
    out[len] = '\0';
    return len;
}

int jt_str(const char *js, const void *tokens, int ntok, int obj_idx,
           const char *key, char *out, size_t out_sz)
{
    int v = jt_get(js, tokens, ntok, obj_idx, key);
    return jt_str_tok(js, tokens, v, out, out_sz);
}

long jt_int(const char *js, const void *tokens, int ntok, int obj_idx,
            const char *key, long def)
{
    int v = jt_get(js, tokens, ntok, obj_idx, key);
    if (v < 0) return def;
    const jsmntok_t *t = &TOKS(tokens)[v];
    if (t->type != JSMN_PRIMITIVE) return def;
    char buf[24];
    int len = t->end - t->start;
    if (len <= 0 || len >= (int)sizeof(buf)) return def;
    memcpy(buf, js + t->start, len);
    buf[len] = '\0';
    // primitivo puo' essere numero, true/false, null: gestiamo il numero
    if (buf[0] == 't' || buf[0] == 'f' || buf[0] == 'n') return def;
    return strtol(buf, NULL, 10);
}

bool jt_bool(const char *js, const void *tokens, int ntok, int obj_idx,
             const char *key, bool def)
{
    int v = jt_get(js, tokens, ntok, obj_idx, key);
    if (v < 0) return def;
    const jsmntok_t *t = &TOKS(tokens)[v];
    if (t->type != JSMN_PRIMITIVE) return def;
    return (js[t->start] == 't');   // "true" inizia per 't'
}

int jt_arr_count(const void *tokens, int arr_idx)
{
    if (arr_idx < 0) return 0;
    const jsmntok_t *t = &TOKS(tokens)[arr_idx];
    if (t->type != JSMN_ARRAY) return 0;
    return t->size;
}

int jt_arr_item(const void *tokens, int ntok, int arr_idx, int i)
{
    const jsmntok_t *t = TOKS(tokens);
    if (arr_idx < 0 || arr_idx >= ntok) return -1;
    if (t[arr_idx].type != JSMN_ARRAY) return -1;
    if (i < 0 || i >= t[arr_idx].size) return -1;
    int j = arr_idx + 1;
    for (int c = 0; c < i; c++) j = skip(t, j);
    return j;
}
