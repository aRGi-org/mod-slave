#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

// Avvia WiFi in APSTA: AP acceso, STA se g_wifi ha credenziali.
void wifi_mgr_start(void);

// Stato STA.
bool wifi_mgr_sta_connected(void);

// Metriche di salute per la diagnostica.
// Numero di volte che la STA ha ottenuto un IP (connessioni + riconnessioni).
uint32_t wifi_mgr_connect_count(void);
// RSSI corrente della STA in dBm (negativo; es. -66). Ritorna 0 se non connessa
// o non disponibile. Valori tipici: > -60 ottimo, -70 buono, < -80 debole.
int wifi_mgr_rssi(void);

// Callback invocata quando la STA ottiene un IP (STA "up").
// Registrala per far reagire altri moduli alla connessione (es. avvio dei
// controller Modbus TCP) senza accoppiare wifi_mgr a quei moduli.
// Viene chiamata a OGNI got IP (anche dopo una riconnessione): il ricevente
// deve gestire da se' l'idempotenza (evitare doppi avvii).
typedef void (*wifi_sta_up_cb_t)(void);
void wifi_mgr_set_sta_up_callback(wifi_sta_up_cb_t cb);

// Stato corrente: true se il gateway e' in STATO AP (configurazione).
bool wifi_mgr_ap_is_on(void);

// Forza il passaggio in STATO AP (pressione breve del pulsante): imposta il flag
// force_ap in NVS e riavvia. Al boot si riparte in AP. Per riconfigurare quando le
// credenziali STA sono cambiate.
void wifi_mgr_ap_request_window(void);

// Netif handles (per captive portal / mDNS futuri).
esp_netif_t *wifi_mgr_get_ap_netif(void);
esp_netif_t *wifi_mgr_get_sta_netif(void);

// Task che sorveglia il pulsante GPIO0 (breve/lungo).
void wifi_button_monitor_start(void);

// ---- Scansione reti WiFi (per il portale di configurazione) ----
// Un record di rete trovata dallo scan. ssid a lunghezza fissa (32+terminatore).
typedef struct {
    char    ssid[33];   // SSID (puo' essere vuoto per reti nascoste)
    int8_t  rssi;       // potenza segnale in dBm (negativo: piu' vicino a 0 = meglio)
    uint8_t channel;    // canale primario
    uint8_t auth_open;  // 1 se rete aperta (WIFI_AUTH_OPEN), 0 se protetta
} wifi_scan_entry_t;

// Esegue una scansione BLOCCANTE delle reti WiFi e riempie 'out' (fino a max_n
// record). Ritorna il numero di reti trovate (>=0), o -1 in caso di errore.
// Pensata per essere chiamata in AP mode dal portale di config: l'AP puo'
// assentarsi brevemente durante lo scan. Ordina per RSSI decrescente (le piu'
// forti per prime) e salta i duplicati di SSID.
int wifi_mgr_scan(wifi_scan_entry_t *out, int max_n);