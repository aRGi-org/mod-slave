// =============================================================================
//  net_time.c - Ora di sistema via NTP/SNTP + timestamp UTC per i log
// =============================================================================
#include "net_time.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_time";

// ---- stato interno ---------------------------------------------------------
static bool s_started   = false;   // SNTP inizializzato
static bool s_enabled   = false;   // NTP abilitato in config
static bool s_synced    = false;   // almeno una sync riuscita
static char s_server[NTP_SERVER_MAXLEN] = {0};
static TaskHandle_t s_resync_task = NULL;

// Soglia per capire se l'ora e' "vera": se time() e' prima di questa data, non e'
// ancora stata sincronizzata (l'orologio parte dal 1970). 2020-01-01 UTC.
#define TIME_VALID_THRESHOLD  1577836800  // 2020-01-01 00:00:00 UTC

// Intervallo di risincronizzazione periodica: ogni ora. Anche il retry dopo un
// fallimento usa lo stesso intervallo (ritenta dopo 1 ora).
#define RESYNC_PERIOD_MS   (60 * 60 * 1000)
// Timeout di attesa della prima sync a ogni tentativo (non bloccare troppo).
#define SYNC_WAIT_MS       (10 * 1000)

// ============================================================================
//  Timestamp dei log in UTC con data
// ============================================================================
//  In ESP-IDF v5.5 NON serve (e NON si deve) sovrascrivere
//  esp_log_system_timestamp(): la funzione della libreria non e' piu' weak
//  (l'override darebbe "multiple definition" al link). Invece si usa l'opzione
//  nativa di menuconfig:
//    Component config -> Log -> Timestamp
//      -> "System time (YY-MM-DD HH:MM:SS.sss)"
//  che stampa data+ora di sistema (es. "2026-08-20 14:31:18.532"). La libreria
//  usa il system time (settato dalla sync NTP qui sotto). I log sono in UTC
//  perche' l'ora di sistema e' UTC (la timezone POSIX serve solo alla
//  visualizzazione locale via localtime, es. nel footer, e NON tocca i log).
//  Nota IDF: i log dei binary blob (WiFi/BT) restano sul tick RTOS.
// ============================================================================

// ============================================================================
//  Timezone
// ============================================================================
void net_time_set_timezone(const char *posix_tz)
{
    if (posix_tz && posix_tz[0]) {
        setenv("TZ", posix_tz, 1);
    } else {
        setenv("TZ", "UTC0", 1);   // default: UTC senza offset
    }
    tzset();
    ESP_LOGI(TAG, "timezone set to '%s' (display only; logs stay UTC)",
             (posix_tz && posix_tz[0]) ? posix_tz : "UTC0");
}

// ============================================================================
//  Callback di avvenuta sincronizzazione
// ============================================================================
static void on_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    char iso[24];
    net_time_utc_iso8601(iso, sizeof(iso));
    ESP_LOGI(TAG, "NTP time synchronized: %s", iso);
}

// ============================================================================
//  Task di risincronizzazione periodica
// ============================================================================
//  esp_netif_sntp gestisce gia' un intervallo interno di refresh, ma teniamo un
//  task nostro per: (1) loggare lo stato, (2) implementare il retry esplicito
//  "riprova dopo 1 ora" anche quando la prima sync fallisce, in modo uniforme e
//  controllato da noi.
static void resync_task(void *arg)
{
    (void)arg;
    for (;;) {
        // Attende l'esito della sync corrente (con timeout, per non restare
        // appeso se la rete non risponde).
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SYNC_WAIT_MS));
        if (err == ESP_OK) {
            if (!s_synced) s_synced = true;
            ESP_LOGD(TAG, "sync ok, next in 1h");
        } else {
            ESP_LOGW(TAG, "NTP sync not completed (%s), retry in 1h",
                     esp_err_to_name(err));
        }
        // In entrambi i casi riproviamo tra un'ora (retry dopo 1h se fallita,
        // refresh periodico se riuscita).
        vTaskDelay(pdMS_TO_TICKS(RESYNC_PERIOD_MS));
        // Richiede una nuova sync per il prossimo giro.
        esp_netif_sntp_start();
    }
}

// ============================================================================
//  Avvio / stop
// ============================================================================
void net_time_start(bool enabled, const char *server, const char *posix_tz)
{
    // La timezone si applica comunque (serve alla visualizzazione locale anche
    // se in questo momento non c'e' ancora l'ora).
    net_time_set_timezone(posix_tz);

    s_enabled = enabled;

    if (!enabled) {
        ESP_LOGI(TAG, "NTP disabled in config; logs use uptime until (if) enabled");
        net_time_stop();
        return;
    }

    if (!server || !server[0]) {
        ESP_LOGW(TAG, "NTP enabled but no server configured; skipping");
        return;
    }

    // Se cambia il server o non era mai partito, (ri)inizializza SNTP.
    bool server_changed = (strncmp(s_server, server, sizeof(s_server)) != 0);
    if (s_started && !server_changed) {
        // Gia' attivo con lo stesso server: forza solo una nuova sync.
        esp_netif_sntp_start();
        ESP_LOGI(TAG, "NTP already running (server '%s'), forcing resync", s_server);
        return;
    }

    // (Ri)avvio pulito.
    if (s_started) {
        esp_netif_sntp_deinit();
        s_started = false;
    }

    strncpy(s_server, server, sizeof(s_server) - 1);
    s_server[sizeof(s_server) - 1] = '\0';

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    cfg.start = true;                       // avvia subito
    cfg.sync_cb = on_sync;                  // callback a sync avvenuta
    cfg.server_from_dhcp = false;           // usiamo solo il server configurato
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed (%s)", esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "NTP started, server '%s'", s_server);

    // Task di risync/retry: creato una sola volta.
    if (s_resync_task == NULL) {
        xTaskCreate(resync_task, "ntp_resync", 3072, NULL, 5, &s_resync_task);
    }
}

void net_time_stop(void)
{
    if (s_started) {
        esp_netif_sntp_deinit();
        s_started = false;
        ESP_LOGI(TAG, "NTP stopped");
    }
    // Il task di risync resta vivo ma innocuo (esp_netif_sntp_start su sistema
    // deinit non fa danni; in alternativa lo si potrebbe fermare). L'ora gia'
    // acquisita resta valida (orologio interno).
}

// ============================================================================
//  Stato / lettura ora
// ============================================================================
bool net_time_is_synced(void)
{
    // Considera sincronizzato se il flag e' set E l'ora e' plausibile.
    return s_synced && (time(NULL) >= TIME_VALID_THRESHOLD);
}

bool net_time_utc_iso8601(char *out, size_t out_len)
{
    if (!out || out_len < 21) {
        if (out && out_len) out[0] = '\0';
        return false;
    }
    time_t now = time(NULL);
    if (now < TIME_VALID_THRESHOLD) {
        out[0] = '\0';
        return false;
    }
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return true;
}

bool net_time_local_hhmm(char *out, size_t out_len)
{
    if (!out || out_len < 17) {
        if (out && out_len) out[0] = '\0';
        return false;
    }
    time_t now = time(NULL);
    if (now < TIME_VALID_THRESHOLD) {
        out[0] = '\0';
        return false;
    }
    struct tm tm_local;
    localtime_r(&now, &tm_local);   // rispetta il TZ impostato con tzset()
    strftime(out, out_len, "%Y-%m-%d %H:%M", &tm_local);
    return true;
}
