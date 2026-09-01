#pragma once
// =============================================================================
//  jt (jsmn tokens) - Helper "cJSON-like" sopra jsmn
// =============================================================================
//  Fornisce funzioni di navigazione simili a cJSON (get object item, get string,
//  get int, iterazione array) lavorando direttamente sull'array di token jsmn,
//  SENZA costruire un albero in RAM. Il consumo di memoria e' solo: la stringa
//  JSON originale (gia' in RAM) + l'array di token (16 byte/token). Molto piu'
//  leggero di cJSON, che duplica l'intero documento in un albero di nodi.
//
//  Uso tipico:
//    jsmntok_t toks[N];
//    int nt = jt_parse(json, len, toks, N);
//    int root = 0;                        // il primo token e' l'oggetto radice
//    int dev = jt_get(json, toks, nt, root, "master_tcp");
//    int arr = jt_get(json, toks, nt, dev, "devices");
//    int n = jt_arr_count(toks, arr);
//    for (int i = 0; i < n; i++) {
//        int item = jt_arr_item(toks, nt, arr, i);
//        int port = jt_int(json, toks, nt, item, "port", 502);
//    }
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Ritorna il numero di token, oppure < 0 su errore jsmn:
//   JSMN_ERROR_NOMEM (-1): array token troppo piccolo
//   JSMN_ERROR_INVAL (-2): JSON malformato
//   JSMN_ERROR_PART  (-3): JSON incompleto
int jt_parse(const char *js, size_t len, void *tokens, unsigned int max_tokens);

// Confronta il token 'tok_idx' (deve essere stringa) con 's'. true se uguale.
bool jt_streq(const char *js, const void *tokens, int tok_idx, const char *s);

// Dentro l'oggetto 'obj_idx', trova il VALORE associato alla chiave 'key'.
// Ritorna l'indice del token valore, o -1 se non trovato. (come GetObjectItem)
int jt_get(const char *js, const void *tokens, int ntok, int obj_idx, const char *key);

// Copia la stringa del token 'val_idx' in 'out' (troncata a out_sz-1). Se il
// token non e' valido, scrive stringa vuota. Ritorna la lunghezza copiata.
int jt_str_tok(const char *js, const void *tokens, int val_idx, char *out, size_t out_sz);

// Come sopra ma cerca prima la chiave dentro obj_idx (comodita').
int jt_str(const char *js, const void *tokens, int ntok, int obj_idx,
           const char *key, char *out, size_t out_sz);

// Legge un intero (primitivo numerico) dalla chiave 'key' dentro obj_idx.
// Se assente, ritorna 'def'.
long jt_int(const char *js, const void *tokens, int ntok, int obj_idx,
            const char *key, long def);

// Legge un booleano JSON (true/false) dalla chiave 'key'. Se assente, 'def'.
bool jt_bool(const char *js, const void *tokens, int ntok, int obj_idx,
             const char *key, bool def);

// Numero di elementi dell'array 'arr_idx' (0 se non e' un array).
int jt_arr_count(const void *tokens, int arr_idx);

// Indice del token dell'i-esimo elemento dell'array 'arr_idx'. -1 se fuori range.
int jt_arr_item(const void *tokens, int ntok, int arr_idx, int i);
