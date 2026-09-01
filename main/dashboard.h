#pragma once
// =============================================================================
//  dashboard - Serializzazione dello store per la Dashboard UI
// =============================================================================
//  Espone il contenuto dello store (holding/input/coils/discrete) in JSON per la
//  Dashboard. I valori sono in HEX. Per holding/input calcola anche una "mappa
//  tipi" per-registro dai blocchi di configurazione:
//    - 16BIT  : registro usato come U16/I16 (verde in UI)
//    - FLOAT  : registro usato come F32/U32/I32 32-bit (giallo in UI)
//    - UNUSED : registro non coperto da alcun blocco (azzurro/default in UI)
//  La UI legge solo la finestra visibile (64 registri) per efficienza.
// =============================================================================

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tipo d'uso di un registro (per il colore in UI).
typedef enum {
    REG_USE_UNUSED = 0,   // nessun blocco lo mappa (azzurro)
    REG_USE_16BIT  = 1,   // U16/I16 (verde)
    REG_USE_FLOAT  = 2,   // F32/U32/I32 - 32 bit su 2 registri (giallo)
} reg_use_t;

// Serializza in JSON una finestra di registri 16-bit (holding o input).
//   area: 0=holding, 1=input
//   start, count: finestra (es. start=0 count=64). Clampato ai limiti store.
// Output JSON: {"area":"holding","start":0,"count":64,
//               "vals":["0x1234",...],"use":[1,2,2,0,...]}
// 'use' ha i codici reg_use_t. Ritorna byte scritti o -1 se buffer piccolo.
int dashboard_regs_json(int area, int start, int count, char *out, size_t out_len);

// Serializza in JSON tutti i bit di un'area (coils o discrete).
//   area: 2=coils, 3=discrete
// Output: {"area":"coils","count":256,"bits":[0,1,0,...]}
// Ritorna byte scritti o -1 se buffer piccolo.
int dashboard_bits_json(int area, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
