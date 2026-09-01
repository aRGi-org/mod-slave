#pragma once
#include <stdbool.h>

// ============================================================================
//  Lingua dell'interfaccia (preferenza globale del device).
//  Namespace NVS dedicato ("lang"): indipendente da config/wifi/auth,
//  sopravvive al factory-reset del pulsante (si azzera solo con erase-flash).
//
//  Valori ammessi: codici a 2 lettere. Default "en".
//    en=inglese, it=italiano, fr=francese, es=spagnolo, de=tedesco
// ============================================================================

#define LANG_CODE_LEN 3   // 2 lettere + terminatore

// Inizializza (carica da NVS o applica default "en").
void app_lang_init(void);

// Ritorna il codice lingua corrente (stringa di 2 lettere, sempre valida).
const char *app_lang_get(void);

// Imposta e salva la lingua. Ritorna false se il codice non e' ammesso.
bool app_lang_set(const char *code);
