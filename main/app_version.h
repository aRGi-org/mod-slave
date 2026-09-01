#pragma once
// =============================================================================
//  app_version - Versione del firmware aRgiModGw
// =============================================================================
//  Versione MANUALE, controllata dallo sviluppatore: incrementala a ogni
//  release significativa. Viene mostrata nella diagnostica (tab Generale) e nel
//  footer della UI, e serve a distinguere i build (utile soprattutto per l'OTA:
//  cosi' si vede subito se l'aggiornamento e' andato a buon fine).
//
//  Accanto a questa, ESP-IDF popola automaticamente esp_app_desc_t (data/ora di
//  build, versione IDF) leggibile con esp_app_get_description(): usiamo entrambe
//  - questo numero per la versione "di prodotto", il build time per il dettaglio.
//
//  Convenzione: MAJOR.MINOR.PATCH
//    - MAJOR: cambi incompatibili / milestone grandi
//    - MINOR: nuove funzionalita' retrocompatibili
//    - PATCH: correzioni
// =============================================================================

#define APP_VERSION_MAJOR   0
#define APP_VERSION_MINOR   1
#define APP_VERSION_PATCH   0

// Stringa derivata automaticamente dalle macro sopra: cosi' non puo' piu'
// disallinearsi (prima era hardcoded "0.0.1" per errore, ora segue MAJOR.MINOR.PATCH).
#define APP_VER_STR2(x) #x
#define APP_VER_STR(x)  APP_VER_STR2(x)
#define APP_VERSION_STRING  APP_VER_STR(APP_VERSION_MAJOR) "." \
                            APP_VER_STR(APP_VERSION_MINOR) "." \
                            APP_VER_STR(APP_VERSION_PATCH)
