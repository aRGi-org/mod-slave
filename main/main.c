#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "reg_store.h"
#include "app_config.h"
#include "app_lang.h"
#include "auth.h"
#include "ota.h"
#include "sys_params.h"
#include "fs_storage.h"
#include "net_mdns.h"
#include "net_time.h"
#include "sys_wdt.h"
#include "event_log.h"
#include "app_version.h"
#include "mb_slave_rtu.h"
#include "data_gen.h"
#include "wifi_mgr.h"
#include "web_server.h"

static const char *TAG = "argimbss";

// ============================================================================
//  main del SIMULATORE SLAVE RTU (fork ridotto di aRgiModGw).
//
//  Rispetto al gateway sparisce tutta la logica TCP: niente tcp_starter_task,
//  niente avvio master/slave TCP alla connessione STA. Il simulatore ha UN solo
//  controller (slave RTU su UART1) + il generatore dati; entrambi partono SEMPRE
//  a boot, indipendenti dal WiFi (deve rispondere sul bus anche senza rete).
//  Si TIENE: infrastruttura (fs, config, wdt, wifi/web, mdns/ntp, event log),
//  OTA rollback (conferma il firmware dopo qualche secondo sano).
// ============================================================================

// Callback quando la STA ottiene un IP. Nel simulatore serve solo ad avviare
// mDNS e NTP (niente controller TCP da far partire). Leggera e non bloccante.
static void on_sta_up(void)
{
    net_mdns_start();
    net_time_start(g_cfg.ntp.enabled, g_cfg.ntp.server, g_cfg.ntp.posix_tz);
    event_log(EVLOG_INFO, "WiFi STA connected, network up");
}

void app_main(void)
{
    // Filesystem LittleFS (partizione 'storage'): montato PRESTO perche'
    // app_config_init() legge da qui le credenziali WiFi di default e la config.
    fs_storage_init();

    reg_store_init();
    sys_wdt_init();          // Task watchdog: armato prima dei task critici

    // Carica configurazione (seriale+phys+datagen) e credenziali WiFi da
    // NVS/LittleFS, applicando i default di fabbrica se assenti.
    app_config_init();

    // Lingua UI (namespace NVS dedicato, default "en").
    app_lang_init();

    // Registro eventi: buffer RAM sempre attivo; persistenza su flash secondo
    // il flag di config (default OFF).
    event_log_init(g_cfg.log_persist);
    event_log(EVLOG_INFO, "argimbss boot (fw %s)", APP_VERSION_STRING);

    // Parametri di sistema (log-level per sottosistema): carica e applica PRESTO.
    sys_params_init();

    // Autenticazione: carica gli utenti da NVS o crea i default.
    auth_init();

    // Registro la callback PRIMA di avviare il WiFi.
    wifi_mgr_set_sta_up_callback(on_sta_up);
    wifi_mgr_start();

    // Monitoraggio pulsante GPIO0: pressione breve -> AP, lunga (>=10s) ->
    // factory reset. Rete di sicurezza per recuperare il device.
    wifi_button_monitor_start();

    web_server_start();

    // Diagnostica: stampa la config caricata come JSON.
    char *cfg_json = app_config_to_json();
    if (cfg_json) {
        ESP_LOGI(TAG, "current config: %s", cfg_json);
        free(cfg_json);
    }

    // Genera i dati nello store PRIMA di avviare lo slave, cosi' i primi accessi
    // del master trovano gia' valori sensati (statici reg[N]=N, oppure il primo
    // seed dell'animazione).
    data_gen_start();

    // Avvia l'unico controller: slave RTU su UART1. Parte SEMPRE, WiFi o no.
    mb_slave_rtu_start();

    ESP_LOGI(TAG, "simulator started: RTU slave active, data generator running");

    // Rollback OTA: dopo qualche secondo di funzionamento sano confermiamo il
    // firmware (disarma il rollback automatico). Se difettoso e crashasse prima
    // di qui, al reboot il bootloader tornerebbe alla versione precedente.
    bool ota_confirmed = false;
    int  healthy_ticks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!ota_confirmed) {
            if (++healthy_ticks >= 10) {   // 10s di uptime stabile
                ota_mark_valid();
                ota_confirmed = true;
            }
        }
    }
}
