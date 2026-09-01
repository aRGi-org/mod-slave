#pragma once
// =============================================================================
//  ota - Aggiornamento firmware Over-The-Air (schema A/B con rollback)
// =============================================================================
//  Il firmware gira su uno di due slot (ota_0 / ota_1). L'aggiornamento scrive
//  sempre sullo slot INATTIVO, poi marca quello come boot al prossimo riavvio.
//  Se il nuovo firmware non si conferma sano (vedi ota_mark_valid), al reboot
//  successivo il bootloader torna automaticamente allo slot precedente
//  (rollback) - richiede CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in menuconfig.
//
//  Due modalita' d'uso:
//   - PUSH: l'handler web riceve il .bin a chunk e chiama ota_begin/write/end.
//   - PULL: ota_from_url() scarica il .bin da un URL (http/https).
//
//  IMPORTANTE (RAM): il .bin e' ~1MB, non entra in RAM. Sempre a chunk:
//  ota_begin() una volta, ota_write() per ogni pezzo, ota_end() alla fine.
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handle opaco di una sessione OTA in corso (push a chunk).
typedef struct ota_session ota_session_t;

// --- PUSH (scrittura a chunk) ------------------------------------------------

// Avvia una sessione OTA: seleziona lo slot inattivo e lo prepara alla scrittura.
// 'total_size' e' la dimensione attesa del binario (0 = sconosciuta/OTA_SIZE_UNKNOWN).
// Ritorna un handle da passare a write/end, oppure NULL in caso di errore
// (con messaggio in out_err se fornito, buffer >= 64 byte).
ota_session_t *ota_begin(size_t total_size, char *out_err, size_t err_len);

// Scrive un chunk sullo slot inattivo. Puo' essere chiamata molte volte.
// Ritorna ESP_OK o un errore (in tal caso la sessione va abortita con ota_abort).
esp_err_t ota_write(ota_session_t *s, const void *data, size_t len);

// Conclude la sessione: valida l'immagine e marca lo slot come boot successivo.
// Se ok, il chiamante dovrebbe riavviare (esp_restart) per attivare il nuovo fw.
// Ritorna ESP_OK oppure errore (out_err popolato).
esp_err_t ota_end(ota_session_t *s, char *out_err, size_t err_len);

// Aborta e libera una sessione in corso senza applicare nulla.
void ota_abort(ota_session_t *s);

// Quanti byte sono stati scritti finora (per progress/log).
size_t ota_written(const ota_session_t *s);

// --- PULL (download da URL) --------------------------------------------------

// Scarica il .bin dall'URL e lo applica (begin/write/end internamente). Bloccante.
// Al successo ritorna ESP_OK e il chiamante dovrebbe riavviare. out_err popolato
// in caso di errore. Supporta http e https (con bundle certificati di sistema).
esp_err_t ota_from_url(const char *url, char *out_err, size_t err_len);

// --- Rollback / stato --------------------------------------------------------

// Da chiamare quando il firmware corrente e' ritenuto SANO (es. dopo boot
// completo: WiFi su, task avviati). Conferma l'immagine e cancella il rollback
// pendente. Se non viene mai chiamata dopo un OTA, al prossimo reboot il
// bootloader torna al firmware precedente. Idempotente e sicura da chiamare
// sempre (se non c'e' rollback pendente, non fa nulla di dannoso).
void ota_mark_valid(void);

// Riempie out_json (buffer del chiamante) con lo stato OTA corrente:
//   { "running": bool, "progress": N, "total": M,
//     "app_version": "...", "running_partition": "ota_0|ota_1|factory",
//     "pending_verify": bool }
// Ritorna il numero di byte scritti (escluso terminatore), o -1 su errore.
int ota_status_json(char *out_json, size_t json_len);

#ifdef __cplusplus
}
#endif
