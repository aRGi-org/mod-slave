#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "reg_store.h"   // per float_layout_t (riuso store)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================================
//  Configurazione runtime del SIMULATORE SLAVE RTU (fork di aRgiModGw).
//
//  Rispetto al gateway e' DRASTICAMENTE ridotta: niente ruoli, niente device,
//  niente blocchi, niente TCP. Il simulatore ha UN SOLO ruolo fisso (slave RTU
//  su UNA porta), quindi la config si riduce a:
//    - parametri seriali della porta + unit_id
//    - modalita' fisica della porta (UART TTL / RS-232 / RS-485 / RS-422)
//    - modalita' del generatore dati (statici / animato)
//
//  Persistenza invariata rispetto al gateway: struct in RAM come verita' a
//  runtime, serializzate in JSON e salvate in NVS. Due namespace: WiFi
//  (cancellabile da pin reset) e config.
// ============================================================================

// ---- Limiti dimensionali ----
#define CFG_SSID_LEN                 33   // 32 + terminatore
#define CFG_PASS_LEN                 65   // 64 + terminatore
#define CFG_NTP_SERVER_LEN           64   // hostname o IP del server NTP
#define CFG_NTP_TZ_LEN               48   // stringa POSIX TZ
#define CFG_NTP_TZNAME_LEN           40   // nome zona IANA (es. Europe/Rome)

// ---- Modalita' fisica della porta (SPEC sez. 4) ----
// Selezionabile via web (NON #define compile-time). Stessi pin TX/RX/DE per
// tutte; cambia solo il transceiver esterno e come il software configura la UART.
typedef enum {
    PHYS_UART_TTL = 0,   // diretto 3.3V, nessun transceiver, DE ignorato
    PHYS_RS232,          // MAX232 (+-12V), solo TX/RX, DE ignorato
    PHYS_RS485,          // MAX485 half-duplex 2 fili A/B, DE = direzione (auto)
    PHYS_RS422           // full-duplex 4 fili, DE tenuto fisso attivo
} port_phys_mode_t;

// ---- Modalita' del generatore dati (SPEC sez. 5) ----
typedef enum {
    DATAGEN_STATIC = 0,  // reg[N]=N: valori riconoscibili per il round-trip
    DATAGEN_ANIM         // 8 forme d'onda sui registri + bit random
} data_gen_mode_t;

// ---- Parametri seriali della porta ----
typedef struct {
    uint32_t baudrate;    // default 38400
    uint8_t  parity;      // 0=none 1=odd 2=even, default 0
    uint8_t  data_bits;   // 7 o 8, default 8
    uint8_t  stop_bits;   // 1 o 2, default 1
    uint8_t  modbus_id;   // 1..247, default 1 (indirizzo slave RTU)
} cfg_serial_t;

// ---- NTP / ora di sistema (invariato dal gateway) ----
typedef struct {
    bool enabled;
    char server[CFG_NTP_SERVER_LEN];
    char posix_tz[CFG_NTP_TZ_LEN];
    char tz_name[CFG_NTP_TZNAME_LEN];
} cfg_ntp_t;

// ---- WiFi (namespace separato, cancellabile da pin reset) ----
typedef struct {
    char ssid[CFG_SSID_LEN];
    char pass[CFG_PASS_LEN];
    bool configured;
    char ap_ssid[CFG_SSID_LEN];
    char ap_pass[CFG_PASS_LEN];
} cfg_wifi_t;

// ---- Config globale del simulatore ----
// Una sola porta, ruolo fisso slave RTU. Niente array ports[], niente TCP.
typedef struct {
    cfg_serial_t     serial;      // baud, parita', data/stop bits, unit_id
    port_phys_mode_t phys_mode;   // modalita' fisica della porta
    data_gen_mode_t  datagen;     // modalita' del generatore dati
    uint32_t         anim_ms;     // periodo di refresh animazione (default 500)
    cfg_ntp_t        ntp;
    bool             log_persist; // event log: persistenza su flash (default OFF)
    bool             web_https;   // server web in HTTPS (default OFF = HTTP)
} cfg_sim_t;

// Istanze globali (definite in app_config.c).
extern cfg_sim_t  g_cfg;
extern cfg_wifi_t g_wifi;

// ---- API ----
void app_config_defaults(void);
void app_config_init(void);
bool app_config_load(void);
bool app_config_save(void);
void app_config_erase(void);   // factory reset: cancella la config persistente

bool app_wifi_load(void);
bool app_wifi_save(void);
bool app_wifi_erase(void);
bool app_wifi_cred_validated_get(void);
bool app_wifi_cred_validated_set(bool v);
bool app_wifi_force_ap_get(void);
bool app_wifi_force_ap_set(bool v);
void app_wifi_defaults(void);

// Serializzazione JSON (usate anche dalla REST API del portale).
char *app_config_to_json(void);
char *app_wifi_to_json(void);   // NB: la password NON viene esposta nel JSON
bool  app_config_from_json(const char *json);
bool  app_wifi_from_json(const char *json);

// Valida l'INTERA config. Ritorna NULL se ok, altrimenti stringa col problema.
const char *app_config_validate_all(const cfg_sim_t *cfg);

// Import atomico da JSON: valida tutto su copia, applica solo se valido.
const char *app_config_import(const char *json);

// True se il baudrate e' nella lista ammessa (RTU_ALLOWED_BAUDS).
bool is_allowed_baud(uint32_t baud);

// Conversione enum <-> stringa (per la serializzazione JSON nella UI).
const char *phys_mode_str(port_phys_mode_t m);
const char *datagen_str(data_gen_mode_t d);

// ============================================================================
//  Protezione concorrenza su g_cfg (mutex).
// ============================================================================
#define CFG_LOCK_TIMEOUT_MS  5000

void cfg_mutex_init(void);
bool cfg_lock(void);    // true se acquisito, false se timeout
void cfg_unlock(void);
