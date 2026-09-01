#pragma once
// =============================================================================
//  fs_storage - Filesystem LittleFS su partizione "storage"
// =============================================================================
//  Monta un LittleFS sulla partizione 'storage' (subtype spiffs, 512K a 0x320000).
//  Serve a conservare file che sopravvivono agli aggiornamenti OTA (l'OTA scrive
//  su ota_0/ota_1, non tocca questa partizione):
//    - /littlefs/wifi.json  -> credenziali WiFi di DEFAULT di fabbrica
//    - /littlefs/cert.pem, /littlefs/key.pem -> (futuro) certificato HTTPS
//
//  L'immagine iniziale con i file di default si genera dalla cartella
//  'littlefs_image/' del progetto e si flasha con idf.py (vedi CMakeLists root).
// =============================================================================

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FS_BASE_PATH   "/littlefs"
#define FS_PARTITION   "storage"

// Monta il LittleFS. Da chiamare una volta all'avvio, presto (prima di leggere
// credenziali). Se la partizione non e' formattata, il componente la formatta.
// Ritorna ESP_OK se montato.
esp_err_t fs_storage_init(void);

// Legge interamente un file di testo dal filesystem in 'out' (NUL-terminato).
// 'path' e' relativo alla base (es. "wifi.json" -> /littlefs/wifi.json).
// Ritorna il numero di byte letti (escluso NUL), 0 se vuoto, -1 se assente/errore.
int fs_read_text(const char *path, char *out, size_t out_len);

// Scrive 'data' (len byte) su un file, sovrascrivendo. Ritorna ESP_OK o errore.
esp_err_t fs_write_text(const char *path, const char *data, size_t len);

// True se il file esiste.
bool fs_exists(const char *path);

// Info spazio (per diagnostica): total/used in byte. Ritorna ESP_OK se disponibili.
esp_err_t fs_info(size_t *total, size_t *used);

// Cancella un file (idempotente: ok anche se non esiste). false solo su errore.
bool fs_delete(const char *path);

#ifdef __cplusplus
}
#endif
