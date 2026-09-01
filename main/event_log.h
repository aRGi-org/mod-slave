#pragma once
// =============================================================================
//  event_log - Registro eventi importanti (buffer circolare RAM + persistenza)
// =============================================================================
//  Registra gli EVENTI SIGNIFICATIVI del gateway (boot, errori, riconnessioni,
//  auto-restart, cambi config, watchdog) - NON tutti i log di sistema, solo
//  quelli che servono a capire "cosa e' successo" dopo un problema in campo.
//
//  DUE LIVELLI, indipendenti:
//   1. Buffer circolare in RAM (EVENT_LOG_CAPACITY eventi): SEMPRE attivo, costa
//      poca RAM. Contiene la cronologia della sessione corrente, visibile nella
//      pagina web. Si perde al riavvio.
//   2. Persistenza su flash (LittleFS): OPZIONALE, controllata dal flag di config
//      (default OFF). Se ON: il buffer viene salvato su flash con scritture
//      MINIMIZZATE (eventi critici subito, minori accumulati) e RICARICATO
//      all'avvio, cosi' la cronologia sopravvive ai riavvii.
//
//  Uso: event_log(EVLOG_INFO, "master TCP restart #%d", n);
//  I timestamp usano l'ora di sistema (UTC, come i log). Se l'ora non e' ancora
//  sincronizzata (NTP), il timestamp e' l'uptime.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Numero di eventi nel buffer circolare RAM.
#define EVENT_LOG_CAPACITY   200
// Lunghezza massima del messaggio di un evento (troncato se piu' lungo).
#define EVENT_LOG_MSG_LEN    96

// Livello/severita' dell'evento. Determina anche il colore nella pagina web:
// ERROR->rosso, WARN->giallo, INFO->verde (altri).
typedef enum {
    EVLOG_INFO = 0,
    EVLOG_WARN = 1,
    EVLOG_ERROR = 2,
} evlog_level_t;

// Un evento registrato.
typedef struct {
    int64_t  ts_unix;                    // epoch UTC (0 se ora non sincronizzata)
    uint32_t uptime_ms;                  // uptime al momento dell'evento
    evlog_level_t level;
    char     msg[EVENT_LOG_MSG_LEN];
} evlog_entry_t;

// Inizializza il registro (buffer RAM). Se 'persist' e' true, prova a ricaricare
// gli eventi salvati su flash. Da chiamare una volta all'avvio, dopo fs_storage.
void event_log_init(bool persist);

// Abilita/disabilita la persistenza su flash a runtime (dal flag di Settings).
// Passando true, il buffer corrente viene subito salvato su flash come baseline.
void event_log_set_persist(bool persist);

// Registra un evento. Thread-safe. Formato printf-like. Il livello determina il
// colore nella UI e se l'evento va scritto su flash SUBITO (ERROR/WARN) o
// accumulato (INFO), quando la persistenza e' attiva.
void event_log(evlog_level_t level, const char *fmt, ...);

// Serializza gli eventi correnti (dal piu' recente al piu' vecchio) in JSON,
// per l'endpoint web. Ritorna una stringa malloc'd (da liberare con free) o NULL.
char *event_log_to_json(void);

// Forza la scrittura su flash del buffer corrente (se persistenza attiva).
// Utile prima di un riavvio pianificato. No-op se persistenza off.
void event_log_flush(void);

#ifdef __cplusplus
}
#endif
