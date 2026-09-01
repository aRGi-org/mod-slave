#include "wifi_mgr.h"
#include "event_log.h"
#include "board_config.h"
#include "app_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "auth.h"
#include "esp_netif.h"

static const char *TAG = "wifi_mgr";

// Callback opzionale invocata quando la STA ottiene un IP (STA "up").
// Permette ad altri moduli (es. avvio dei controller TCP) di reagire alla
// connessione senza che wifi_mgr li conosca: wifi_mgr resta agnostico.
static wifi_sta_up_cb_t s_sta_up_cb = NULL;

void wifi_mgr_set_sta_up_callback(wifi_sta_up_cb_t cb)
{
    s_sta_up_cb = cb;
}

static esp_netif_t *s_ap_netif  = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_ap_mode = false;             // true = STATO AP, false = STATO STA
static bool s_sta_connected = false;
static uint32_t s_sta_connect_count = 0;   // quante volte la STA ha ottenuto un IP

// --- Macchina a stati STA: contatore tentativi e backoff ---
// FASE 1 (cred_validated=false): 6 tentativi a 10s fissi, poi reboot in AP.
// FASE 2 (cred_validated=true, blackout): retry infinito con rampa 10..60s.
#define STA_PHASE1_MAX_ATTEMPTS   6
#define STA_PHASE1_INTERVAL_MS    10000
#define STA_BLACKOUT_START_MS     10000
#define STA_BLACKOUT_STEP_MS      10000
#define STA_BLACKOUT_MAX_MS       60000
static int  s_sta_attempts = 0;            // tentativi falliti nella sessione corrente
static bool s_cred_validated = false;      // copia in RAM del flag NVS
static esp_timer_handle_t s_sta_retry_timer = NULL;  // ritenta dopo intervallo

// ============================================================================
//  MACCHINA A STATI STA (retry con backoff, reboot in AP)
// ============================================================================

// Calcola l'intervallo prima del prossimo tentativo, in ms.
// FASE 1 (cred non validate): fisso. FASE 2 (blackout): rampa lineare 10..60s.
static uint32_t sta_next_interval_ms(void)
{
    if (!s_cred_validated) {
        return STA_PHASE1_INTERVAL_MS;
    }
    // Blackout: rampa 10,20,30,...,60 poi costante. s_sta_attempts parte da 1.
    uint32_t ms = STA_BLACKOUT_START_MS + (uint32_t)(s_sta_attempts - 1) * STA_BLACKOUT_STEP_MS;
    if (ms > STA_BLACKOUT_MAX_MS) ms = STA_BLACKOUT_MAX_MS;
    return ms;
}

// Callback del timer di retry: ritenta la connessione STA.
static void sta_retry_cb(void *arg)
{
    ESP_LOGI(TAG, "STA retry (attempt %d)", s_sta_attempts);
    esp_wifi_connect();
}

// Programma il prossimo tentativo di connessione dopo l'intervallo calcolato.
static void sta_schedule_retry(void)
{
    if (!s_sta_retry_timer) { esp_wifi_connect(); return; }
    uint32_t ms = sta_next_interval_ms();
    esp_timer_stop(s_sta_retry_timer);
    esp_timer_start_once(s_sta_retry_timer, (uint64_t)ms * 1000);
    ESP_LOGI(TAG, "next STA attempt in %u ms (validated=%d, attempts=%d)",
             (unsigned)ms, (int)s_cred_validated, s_sta_attempts);
}

// Entra in STATO AP tramite reboot: imposta il flag one-shot e riavvia.
static void reboot_into_ap(const char *reason)
{
    ESP_LOGW(TAG, "switching to AP mode via reboot: %s", reason);
    event_log(EVLOG_WARN, "WiFi -> AP mode (%s), rebooting", reason);
    app_wifi_force_ap_set(true);
    vTaskDelay(pdMS_TO_TICKS(300));   // lascia scrivere NVS e defluire i log
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            // In STATO AP la STA non e' attiva: ignora eventuali eventi residui.
            if (s_ap_mode) break;

            // Logga solo la transizione connesso->disconnesso (i retry falliti
            // scattano di continuo e intaserebbero l'event log).
            if (s_sta_connected) {
                event_log(EVLOG_WARN, "WiFi STA disconnected");
            }
            s_sta_connected = false;
            s_sta_attempts++;
            ESP_LOGW(TAG, "STA disconnected (attempt %d, validated=%d)",
                     s_sta_attempts, (int)s_cred_validated);

            if (!s_cred_validated) {
                // FASE 1: credenziali mai validate. Dopo N tentativi -> AP.
                if (s_sta_attempts >= STA_PHASE1_MAX_ATTEMPTS) {
                    reboot_into_ap("credentials not validated");
                    break;   // non raggiunto (reboot), per chiarezza
                }
            }
            // FASE 2 (blackout) o FASE 1 non ancora esaurita: ritenta col backoff.
            sta_schedule_retry();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "a client connected to the AP");
            event_log(EVLOG_INFO, "AP: client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "a client disconnected from the AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_sta_connect_count++;
        s_sta_attempts = 0;   // reset backoff

        // Primo successo con queste credenziali: marca come validate (NVS).
        if (!s_cred_validated) {
            s_cred_validated = true;
            app_wifi_cred_validated_set(true);
            ESP_LOGI(TAG, "credentials validated (saved to NVS)");
            event_log(EVLOG_INFO, "WiFi credentials validated");
        }

        // Notifica chi si e' registrato (es. avvio controller TCP). Idempotente
        // lato ricevente (puo' arrivare a ogni riconnessione). L'evento
        // "STA connected" viene loggato dalla callback (on_sta_up in main).
        if (s_sta_up_cb) s_sta_up_cb();
    }
}

// ============================================================================
//  INIT AP / STA
// ============================================================================

static void wifi_apply_ap_config(void)
{
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, g_wifi.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(g_wifi.ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    if (g_wifi.ap_pass[0] != '\0') {
        strncpy((char *)ap.ap.password, g_wifi.ap_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

static void wifi_apply_sta_config(void)
{
    if (!g_wifi.configured || g_wifi.ssid[0] == '\0') return;
    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, g_wifi.ssid, sizeof(sta.sta.ssid));
    strncpy((char *)sta.sta.password, g_wifi.pass, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

void wifi_mgr_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // Configura l'IP dell'AP dai define (invece del default ESP-IDF).
    {
        esp_netif_ip_info_t ip_info;
        esp_netif_str_to_ip4(AP_IP_ADDR, &ip_info.ip);
        esp_netif_str_to_ip4(AP_IP_ADDR, &ip_info.gw);   // gateway = se stesso
        esp_netif_str_to_ip4(AP_NETMASK, &ip_info.netmask);

        esp_netif_dhcps_stop(s_ap_netif);                 // ferma DHCP per cambiare IP
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        esp_netif_dhcps_start(s_ap_netif);                // riavvia DHCP con la nuova subnet
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // --- Decide il modo iniziale (ESCLUSIVO: mai AP+STA insieme) ---
    // AP se: flag force_ap (one-shot), oppure nessuna credenziale STA salvata.
    // Altrimenti STA. Il flag force_ap viene consumato (azzerato) entrando in AP.
    bool force_ap = app_wifi_force_ap_get();
    s_cred_validated = app_wifi_cred_validated_get();
    bool go_ap = force_ap || !g_wifi.configured || g_wifi.ssid[0] == '\0';

    if (go_ap) {
        s_ap_mode = true;
        if (force_ap) app_wifi_force_ap_set(false);   // consuma il one-shot
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        wifi_apply_ap_config();
        ESP_LOGI(TAG, "starting in AP mode (force_ap=%d, configured=%d)",
                 (int)force_ap, (int)g_wifi.configured);
    } else {
        s_ap_mode = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_apply_sta_config();
        ESP_LOGI(TAG, "starting in STA mode (ssid='%s', validated=%d)",
                 g_wifi.ssid, (int)s_cred_validated);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // Massime performance di rete: disabilita il modem power save (latenza Modbus).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Timer per il retry STA con backoff (one-shot, riarmato ad ogni tentativo).
    const esp_timer_create_args_t targs = {
        .callback = &sta_retry_cb,
        .name = "sta_retry"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_sta_retry_timer));

    event_log(EVLOG_INFO, s_ap_mode ? "WiFi started in AP mode"
                                    : "WiFi started in STA mode");
    ESP_LOGI(TAG, "WiFi started (%s)", s_ap_mode ? "AP" : "STA");
}

// ============================================================================
//  CONTROLLO STATO (AP/STA esclusivi)
// ============================================================================

bool wifi_mgr_ap_is_on(void)      { return s_ap_mode; }
bool wifi_mgr_sta_connected(void) { return s_sta_connected; }

// Forza il passaggio in STATO AP (button breve). Con il modello a reboot: imposta
// il flag one-shot force_ap e riavvia; al boot si parte in AP. Serve per
// riconfigurare quando le credenziali sono CAMBIATE (la STA insisterebbe su
// credenziali obsolete all'infinito).
void wifi_mgr_ap_request_window(void)
{
    ESP_LOGI(TAG, "AP forced by button: rebooting into AP mode");
    reboot_into_ap("button short press");
}

esp_netif_t *wifi_mgr_get_ap_netif(void)  { return s_ap_netif; }
esp_netif_t *wifi_mgr_get_sta_netif(void) { return s_sta_netif; }

// ============================================================================
//  MONITOR PULSANTE GPIO0 (breve = risveglia AP, lungo = reset WiFi)
// ============================================================================

// Legge il pulsante con debounce: ritorna true solo se il livello richiesto
// e' stabile per DEBOUNCE_SAMPLES letture consecutive.
static bool button_stable_level(int want_level)
{
    const int DEBOUNCE_SAMPLES = 5;
    const int SAMPLE_MS = 10;
    for (int i = 0; i < DEBOUNCE_SAMPLES; i++) {
        if (gpio_get_level(WIFI_BTN_GPIO) != want_level) return false;
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
    return true;
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WIFI_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // a riposo HIGH, premuto LOW
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    while (1) {
        // Rileva pressione stabile (LOW confermato per debounce).
        if (gpio_get_level(WIFI_BTN_GPIO) == 0 && button_stable_level(0)) {
            int64_t t0 = esp_timer_get_time();
            bool reset_triggered = false;

            // Attendi rilascio STABILE (HIGH confermato) o soglia di reset.
            while (1) {
                if (gpio_get_level(WIFI_BTN_GPIO) == 1 && button_stable_level(1)) {
                    break;   // rilasciato (debounced)
                }
                int64_t held_ms = (esp_timer_get_time() - t0) / 1000;
                if (held_ms >= WIFI_RESET_HOLD_MS) {
                    reset_triggered = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (reset_triggered) {
                ESP_LOGW(TAG, "LONG press: factory-reset (WiFi + users + config) and reboot");
                app_wifi_erase();
                auth_erase();
                app_config_erase();   // config su LittleFS: va cancellata anche
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            } else {
                int64_t held_ms = (esp_timer_get_time() - t0) / 1000;
                if (held_ms < WIFI_SHORT_PRESS_MAX_MS) {
                    ESP_LOGI(TAG, "SHORT press: waking up AP");
                    wifi_mgr_ap_request_window();
                } else {
                    ESP_LOGI(TAG, "dead-zone press (%lld ms): ignored", held_ms);
                }
            }

            // Piccola pausa anti-ripetizione dopo il rilascio.
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void wifi_button_monitor_start(void)
{
    xTaskCreate(button_task, "wifi_btn", 3072, NULL, 5, NULL);
}
// ---- Metriche di salute per la diagnostica ----
uint32_t wifi_mgr_connect_count(void)
{
    return s_sta_connect_count;
}

int wifi_mgr_rssi(void)
{
    if (!s_sta_connected) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

// ---- Scansione reti WiFi (bloccante) per il portale di configurazione ----
int wifi_mgr_scan(wifi_scan_entry_t *out, int max_n)
{
    if (!out || max_n <= 0) return -1;

    // Scan bloccante su tutti i canali. In AP mode la radio si assenta per la
    // durata dello scan; l'AP si ripristina al termine.
    wifi_scan_config_t sc = { 0 };   // tutti i canali, scan attivo, tutti gli SSID
    esp_err_t err = esp_wifi_scan_start(&sc, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed to start: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) return 0;

    // Recupera i record in un buffer temporaneo.
    wifi_ap_record_t *recs = calloc(found, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_clear_ap_list(); return -1; }
    uint16_t got = found;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    // Copia nei record di uscita saltando i duplicati di SSID (tiene il primo,
    // che grazie all'ordinamento per RSSI e' il piu' forte) e le reti senza SSID.
    // I risultati di esp_wifi_scan_get_ap_records sono gia' ordinati per RSSI.
    int n = 0;
    for (int i = 0; i < got && n < max_n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') continue;   // salta reti nascoste/senza SSID

        // dedup: gia' presente in out?
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (strncmp(out[j].ssid, ssid, sizeof(out[j].ssid)) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        strncpy(out[n].ssid, ssid, sizeof(out[n].ssid) - 1);
        out[n].ssid[sizeof(out[n].ssid) - 1] = '\0';
        out[n].rssi = recs[i].rssi;
        out[n].channel = recs[i].primary;
        out[n].auth_open = (recs[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
        n++;
    }

    free(recs);
    ESP_LOGI(TAG, "wifi scan: %d networks (%d after dedup)", (int)found, n);
    return n;
}
