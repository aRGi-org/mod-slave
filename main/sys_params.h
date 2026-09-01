#pragma once
// =============================================================================
//  sys_params - Parametri di sistema configurabili (persistenti in NVS, UI)
// =============================================================================
//  Primo cliente: il LIVELLO DI LOG per sottosistema. L'infrastruttura e' pensata
//  per ospitare altri parametri di sistema in futuro (es. diag_refresh_ms) senza
//  reinventare persistenza/UI.
//
//  LOG-LEVEL PER SOTTOSISTEMA
//  Ogni "sottosistema" raggruppa piu' TAG di log (nostri + interni esp-modbus).
//  Un livello (0-5) per sottosistema; al boot e a ogni modifica si applica con
//  esp_log_level_set() a tutti i TAG del gruppo. Cambio a RUNTIME immediato.
//  DEFAULT DI FABBRICA: tutti a NONE (silenzio totale) - in campo nessuno e'
//  collegato al seriale; chi diagnostica alza il livello del sottosistema voluto.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// I sottosistemi di log configurabili. L'ordine e' l'indice in log_levels[].
typedef enum {
    LOG_SUBSYS_RTU_COM1 = 0,   // master_rtu (+ seriale COM1)
    LOG_SUBSYS_RTU_COM2,       // slave_rtu
    LOG_SUBSYS_MASTER_TCP,     // master_tcp, mb_port.tcp.master, mbc_tcp.master
    LOG_SUBSYS_SLAVE_TCP,      // slave_tcp, mb_port.tcp.slave, mbc_tcp.slave
    LOG_SUBSYS_WEB,            // web, auth, session
    LOG_SUBSYS_WIFI,           // wifi_mgr
    LOG_SUBSYS_DIAG,           // mb_diag
    LOG_SUBSYS_SYSTEM,         // main, mb_hub, app_config, app_lang, ota
    LOG_SUBSYS_COUNT
} log_subsys_t;

// Livelli ammessi (mappano su esp_log_level_t):
//   0=NONE 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=VERBOSE
// Usiamo direttamente i valori di esp_log_level_t (ESP_LOG_NONE=0 ... VERBOSE=5).

// Inizializza il modulo: carica i parametri da NVS (o applica i default) e li
// APPLICA subito (esp_log_level_set su tutti i TAG). Da chiamare presto in app_main.
void sys_params_init(void);

// Ritorna il livello corrente di un sottosistema (0-5).
uint8_t sys_log_level_get(log_subsys_t s);

// Imposta il livello di un sottosistema: aggiorna lo stato, lo APPLICA subito a
// runtime (esp_log_level_set su tutti i TAG del gruppo) e SALVA in NVS.
// Ritorna true se ok (indice e livello validi).
bool sys_log_level_set(log_subsys_t s, uint8_t level);

// Serializza lo stato dei log-level in JSON per la UI:
//   {"log_levels":[{"key":"rtu_com1","name":"RTU COM1","level":0}, ...]}
// Ritorna byte scritti (escluso terminatore) o -1 se il buffer e' troppo piccolo.
int sys_params_log_json(char *out, size_t out_len);

// Applica un JSON dalla UI: {"log_levels":[{"key":"...","level":N}, ...]}.
// Aggiorna solo le chiavi presenti e valide; applica e salva. Ritorna true se
// almeno un valore valido e' stato applicato.
bool sys_params_log_apply_json(const char *json);

// Nome/chiave testuale di un sottosistema (per la UI e i log).
const char *sys_log_subsys_key(log_subsys_t s);   // es. "master_tcp"
const char *sys_log_subsys_name(log_subsys_t s);  // es. "Master TCP"

#ifdef __cplusplus
}
#endif
