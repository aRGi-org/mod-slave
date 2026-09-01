#include "app_config.h"
#include "event_log.h"
#include "board_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#define JSMN_HEADER
#include "jsmn.h"
#include "jt_helper.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "fs_storage.h"

// ============================================================================
//  Configurazione runtime del SIMULATORE SLAVE RTU (fork di aRgiModGw).
//
//  Fork del gateway RIDOTTO: monoporta, ruolo fisso slave RTU. Rispetto al
//  gateway si TOLGONO ruoli, device, blocchi e tutto il TCP; si TIENE tutto il
//  resto (stile jsmn/cJSON, persistenza su file LittleFS + migrazione NVS,
//  import/export). Si AGGIUNGONO: modalita' fisica della porta (UART/RS232/
//  RS422/RS485) e modalita' del generatore dati (statici/animato).
//
//  Persistenza IDENTICA al gateway: config serializzata in JSON e salvata come
//  FILE su LittleFS (niente limite 4KB di NVS; e lascia spazio a crescere).
//  NVS resta per WiFi e flag. Due namespace: WiFi (cancellabile da pin reset)
//  e config (per l'eventuale vecchio blob da migrare).
// ============================================================================

static const char *TAG = "app_config";

// Istanze globali.
cfg_sim_t  g_cfg;
cfg_wifi_t g_wifi;

// Mutex a protezione di g_cfg.
static SemaphoreHandle_t s_cfg_mutex = NULL;

// Namespace e chiavi NVS.
#define NVS_NS_CONFIG   "config"
#define NVS_NS_WIFI     "wifi"
#define NVS_KEY_JSON    "json"
#define NVS_KEY_WIFI    "json"

// La config e' salvata come FILE su LittleFS (come nel gateway). Percorso
// relativo a FS_BASE_PATH ("/littlefs"). NVS resta per i dati piccoli.
#define CFG_FILE_PATH   "config.json"
// Config del simulatore piccola, ma teniamo margine abbondante come il gateway.
#define CFG_FILE_MAX    8192

// ============================================================================
//  ENUM <-> STRINGA (modalita' fisica + generatore dati)
// ============================================================================

const char *phys_mode_str(port_phys_mode_t m)
{
    switch (m) {
        case PHYS_RS232: return "rs232";
        case PHYS_RS485: return "rs485";
        case PHYS_RS422: return "rs422";
        case PHYS_UART_TTL:
        default:         return "uart";
    }
}
static port_phys_mode_t phys_mode_parse(const char *s)
{
    if (!s) return PHYS_UART_TTL;
    if (!strcmp(s, "rs232")) return PHYS_RS232;
    if (!strcmp(s, "rs485")) return PHYS_RS485;
    if (!strcmp(s, "rs422")) return PHYS_RS422;
    return PHYS_UART_TTL;
}

const char *datagen_str(data_gen_mode_t d)
{
    return (d == DATAGEN_ANIM) ? "anim" : "static";
}
static data_gen_mode_t datagen_parse(const char *s)
{
    return (s && !strcmp(s, "anim")) ? DATAGEN_ANIM : DATAGEN_STATIC;
}

// ============================================================================
//  DEFAULT DI FABBRICA
// ============================================================================

// Default seriali: 38400 8N1, unit_id 1 (allineati al gateway RTU, SPEC sez.6).
static void serial_defaults(cfg_serial_t *s)
{
    s->baudrate  = SIM_UART_BAUD;
    s->parity    = 0;   // none
    s->data_bits = 8;
    s->stop_bits = 1;
    s->modbus_id = SIM_SLAVE_ADDR;
}

void app_config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    serial_defaults(&g_cfg.serial);
    g_cfg.phys_mode = PHYS_UART_TTL;   // primi test: TTL diretto (SPEC sez. 4/8)
    g_cfg.datagen   = DATAGEN_STATIC;  // statici per primi (SPEC sez. 5)
    g_cfg.anim_ms   = 500;             // tick animazione (SPEC sez. 5)

    // NTP: abilitato, pool europeo, Europa Centrale con ora legale (dal gateway).
    g_cfg.ntp.enabled = true;
    strncpy(g_cfg.ntp.server, "0.europe.pool.ntp.org", CFG_NTP_SERVER_LEN - 1);
    strncpy(g_cfg.ntp.posix_tz, "CET-1CEST,M3.5.0,M10.5.0/3", CFG_NTP_TZ_LEN - 1);
    strncpy(g_cfg.ntp.tz_name, "Europe/Rome", CFG_NTP_TZNAME_LEN - 1);

    g_cfg.log_persist = false;
    g_cfg.web_https   = false;   // default HTTP; HTTPS attivabile da admin via web
}

void app_wifi_defaults(void)
{
    memset(&g_wifi, 0, sizeof(g_wifi));

    // AP di default: SSID del captive portal (password vuota = AP aperto).
    strncpy(g_wifi.ap_ssid, "argimbss", CFG_SSID_LEN - 1);
    g_wifi.ap_pass[0] = '\0';
    g_wifi.configured = false;

    // Credenziali STA di DEFAULT lette da LittleFS (/littlefs/wifi.json), se
    // presenti (precaricate in fabbrica, sopravvivono agli OTA). Se assenti,
    // parte il captive portal. Le credenziali salvate dal portale (NVS) hanno
    // la precedenza su questi default.
    char buf[256];
    int n = fs_read_text("wifi.json", buf, sizeof(buf));
    if (n > 0) {
        cJSON *j = cJSON_Parse(buf);
        if (j) {
            cJSON *s = cJSON_GetObjectItem(j, "ssid");
            cJSON *p = cJSON_GetObjectItem(j, "pass");
            if (cJSON_IsString(s) && s->valuestring[0]) {
                strncpy(g_wifi.ssid, s->valuestring, CFG_SSID_LEN - 1);
                if (cJSON_IsString(p) && p->valuestring[0])
                    strncpy(g_wifi.pass, p->valuestring, CFG_PASS_LEN - 1);
                g_wifi.configured = true;
                ESP_LOGI(TAG, "default WiFi credentials loaded from LittleFS");
            }
            cJSON_Delete(j);
        }
    }
}

// ============================================================================
//  SERIALIZZAZIONE JSON (config)
// ============================================================================

static cJSON *serial_to_json(const cfg_serial_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "baudrate", s->baudrate);
    cJSON_AddNumberToObject(o, "parity", s->parity);
    cJSON_AddNumberToObject(o, "data_bits", s->data_bits);
    cJSON_AddNumberToObject(o, "stop_bits", s->stop_bits);
    cJSON_AddNumberToObject(o, "modbus_id", s->modbus_id);
    return o;
}

static void serial_from_json(const char *js, const void *toks, int ntok, int o, cfg_serial_t *s)
{
    // Default se assente: 38400 8N1 id 1.
    s->baudrate = 38400; s->parity = 0; s->data_bits = 8; s->stop_bits = 1; s->modbus_id = 1;
    if (o < 0) return;
    s->baudrate  = (uint32_t)jt_int(js, toks, ntok, o, "baudrate",  s->baudrate);
    s->parity    = (uint8_t) jt_int(js, toks, ntok, o, "parity",    s->parity);
    s->data_bits = (uint8_t) jt_int(js, toks, ntok, o, "data_bits", s->data_bits);
    s->stop_bits = (uint8_t) jt_int(js, toks, ntok, o, "stop_bits", s->stop_bits);
    s->modbus_id = (uint8_t) jt_int(js, toks, ntok, o, "modbus_id", s->modbus_id);
}

static cJSON *ntp_to_json(const cfg_ntp_t *n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", n->enabled);
    cJSON_AddStringToObject(o, "server", n->server);
    cJSON_AddStringToObject(o, "tz", n->posix_tz);
    cJSON_AddStringToObject(o, "tz_name", n->tz_name);
    return o;
}

static void ntp_from_json(const char *js, const void *toks, int ntok, int o, cfg_ntp_t *n)
{
    if (o < 0) return;
    n->enabled = jt_bool(js, toks, ntok, o, "enabled", true);
    jt_str(js, toks, ntok, o, "server", n->server, CFG_NTP_SERVER_LEN);
    jt_str(js, toks, ntok, o, "tz", n->posix_tz, CFG_NTP_TZ_LEN);
    jt_str(js, toks, ntok, o, "tz_name", n->tz_name, CFG_NTP_TZNAME_LEN);
}

char *app_config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "serial",    serial_to_json(&g_cfg.serial));
    cJSON_AddStringToObject(root, "phys_mode", phys_mode_str(g_cfg.phys_mode));
    cJSON_AddStringToObject(root, "datagen",   datagen_str(g_cfg.datagen));
    cJSON_AddNumberToObject(root, "anim_ms",   g_cfg.anim_ms);
    cJSON_AddItemToObject(root, "ntp",        ntp_to_json(&g_cfg.ntp));
    cJSON_AddBoolToObject(root, "log_persist", g_cfg.log_persist);
    cJSON_AddBoolToObject(root, "web_https", g_cfg.web_https);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;   // il chiamante fa free()
}

bool app_config_from_json(const char *json)
{
    if (!json) return false;
    size_t len = strlen(json);

    // Passata 1: conto i token SENZA allocare (jsmn con tokens=NULL). Stile
    // gateway: dimensiono l'array esatto invece di un blocco fisso grande.
    int need = jt_parse(json, len, NULL, 0);
    if (need < 0) {
        if (need == JSMN_ERROR_INVAL || need == JSMN_ERROR_PART)
            ESP_LOGW(TAG, "config parse: malformed JSON (err %d)", need);
        else
            ESP_LOGW(TAG, "config parse: count failed (err %d)", need);
        return false;
    }
    if (need == 0) {
        ESP_LOGW(TAG, "config parse: empty JSON");
        return false;
    }

    size_t ntokens = (size_t)need + 1;
    jsmntok_t *toks = malloc(sizeof(jsmntok_t) * ntokens);
    if (!toks) {
        ESP_LOGW(TAG, "config parse: out of memory for %u tokens", (unsigned)ntokens);
        return false;
    }

    // Passata 2: parsing vero nell'array dimensionato.
    int ntok = jt_parse(json, len, toks, ntokens);
    if (ntok < 0) {
        ESP_LOGW(TAG, "config parse: parse failed (err %d)", ntok);
        free(toks);
        return false;
    }
    if (ntok == 0 || toks[0].type != JSMN_OBJECT) {
        ESP_LOGW(TAG, "config parse: root is not an object");
        free(toks);
        return false;
    }

    int root = 0;
    serial_from_json(json, toks, ntok, jt_get(json, toks, ntok, root, "serial"), &g_cfg.serial);

    char tmp[16];
    jt_str(json, toks, ntok, root, "phys_mode", tmp, sizeof(tmp));
    g_cfg.phys_mode = phys_mode_parse(tmp);
    jt_str(json, toks, ntok, root, "datagen", tmp, sizeof(tmp));
    g_cfg.datagen = datagen_parse(tmp);
    g_cfg.anim_ms = (uint32_t)jt_int(json, toks, ntok, root, "anim_ms", 500);

    ntp_from_json(json, toks, ntok, jt_get(json, toks, ntok, root, "ntp"), &g_cfg.ntp);
    g_cfg.log_persist = jt_bool(json, toks, ntok, root, "log_persist", false);
    g_cfg.web_https   = jt_bool(json, toks, ntok, root, "web_https", false);

    free(toks);
    return true;
}

// ============================================================================
//  SERIALIZZAZIONE JSON (WiFi) - identica al gateway
// ============================================================================

char *app_wifi_to_json(void)
{
    // La password STA e AP NON vengono esposte (solo un flag "has_pass").
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", g_wifi.ssid);
    cJSON_AddBoolToObject(root, "has_pass", g_wifi.pass[0] != '\0');
    cJSON_AddBoolToObject(root, "configured", g_wifi.configured);
    cJSON_AddStringToObject(root, "ap_ssid", g_wifi.ap_ssid);
    cJSON_AddBoolToObject(root, "ap_has_pass", g_wifi.ap_pass[0] != '\0');
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool app_wifi_from_json(const char *json)
{
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    const cJSON *v;
    v = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(v)) { strncpy(g_wifi.ssid, v->valuestring, CFG_SSID_LEN - 1); g_wifi.ssid[CFG_SSID_LEN - 1] = '\0'; }
    // La password si aggiorna SOLO se presente e non vuota (la UI non la reinvia
    // se non cambiata).
    v = cJSON_GetObjectItem(root, "pass");
    if (cJSON_IsString(v) && v->valuestring[0] != '\0') { strncpy(g_wifi.pass, v->valuestring, CFG_PASS_LEN - 1); g_wifi.pass[CFG_PASS_LEN - 1] = '\0'; }
    v = cJSON_GetObjectItem(root, "ap_ssid");
    if (cJSON_IsString(v)) { strncpy(g_wifi.ap_ssid, v->valuestring, CFG_SSID_LEN - 1); g_wifi.ap_ssid[CFG_SSID_LEN - 1] = '\0'; }
    v = cJSON_GetObjectItem(root, "ap_pass");
    if (cJSON_IsString(v) && v->valuestring[0] != '\0') { strncpy(g_wifi.ap_pass, v->valuestring, CFG_PASS_LEN - 1); g_wifi.ap_pass[CFG_PASS_LEN - 1] = '\0'; }
    g_wifi.configured = (g_wifi.ssid[0] != '\0');
    cJSON_Delete(root);
    return true;
}

// ============================================================================
//  PERSISTENZA NVS (helper) + FILE LittleFS (config) - come il gateway
// ============================================================================

static esp_err_t nvs_save_string_err(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open %s: %s", ns, esp_err_to_name(err)); return err; }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_set %s/%s: %s", ns, key, esp_err_to_name(err)); }
    return err;
}

static bool nvs_save_string(const char *ns, const char *key, const char *val)
{
    return nvs_save_string_err(ns, key, val) == ESP_OK;
}

static char *nvs_load_string(const char *ns, const char *key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) { nvs_close(h); return NULL; }
    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return NULL; }
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { free(buf); return NULL; }
    return buf;
}

bool app_config_save(void)
{
    char *json = app_config_to_json();
    if (!json) return false;
    size_t len = strlen(json);

    // Scrittura su file LittleFS (come il gateway; niente limite 4KB di NVS).
    esp_err_t err = fs_write_text(CFG_FILE_PATH, json, len);
    free(json);
    bool ok = (err == ESP_OK);

    ESP_LOGI(TAG, "config saved (%s, %u bytes)", ok ? "ok" : "FAIL", (unsigned)len);
    if (ok) event_log(EVLOG_INFO, "configuration saved");
    return ok;
}

// Cancella la vecchia chiave config da NVS (dopo la migrazione a file).
static void nvs_erase_key_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_JSON);
    nvs_commit(h);
    nvs_close(h);
}

// Factory reset della config: cancella il file su LittleFS e l'eventuale
// vecchio blob in NVS. Chiamata dal factory-reset (pressione lunga del pulsante).
void app_config_erase(void)
{
    fs_delete(CFG_FILE_PATH);
    nvs_erase_key_config();
    ESP_LOGI(TAG, "config erased (file + nvs)");
}

bool app_config_load(void)
{
    // Applica sempre PRIMA i default: i campi nuovi ereditano il default invece
    // di restare a zero. Poi si sovrascrive con quanto trovato nel file/NVS.
    app_config_defaults();

    char *buf = malloc(CFG_FILE_MAX);
    if (!buf) { ESP_LOGE(TAG, "config load: out of memory"); return false; }

    // 1) Percorso normale: config su file LittleFS.
    int n = fs_read_text(CFG_FILE_PATH, buf, CFG_FILE_MAX);
    if (n > 0) {
        bool ok = app_config_from_json(buf);
        free(buf);
        if (!ok) { app_config_defaults(); return false; }
        ESP_LOGI(TAG, "config loaded from file (%d bytes)", n);
        return true;
    }

    // 2) MIGRAZIONE: nessun file, ma forse una vecchia config in NVS (firmware
    // precedente). La leggo, applico, ri-salvo su file e pulisco NVS.
    char *nvs_json = nvs_load_string(NVS_NS_CONFIG, NVS_KEY_JSON);
    if (nvs_json) {
        bool ok = app_config_from_json(nvs_json);
        free(nvs_json);
        if (ok) {
            ESP_LOGW(TAG, "migrating config from NVS to file");
            app_config_save();
            nvs_erase_key_config();
            free(buf);
            return true;
        }
        app_config_defaults();
    }

    // 3) Niente file, niente NVS: default.
    free(buf);
    ESP_LOGI(TAG, "no config file, using defaults");
    return false;
}

// La password STA/AP viene salvata a parte, in chiaro, in NVS (non esposto).
static char *wifi_full_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", g_wifi.ssid);
    cJSON_AddStringToObject(root, "pass", g_wifi.pass);
    cJSON_AddBoolToObject(root, "configured", g_wifi.configured);
    cJSON_AddStringToObject(root, "ap_ssid", g_wifi.ap_ssid);
    cJSON_AddStringToObject(root, "ap_pass", g_wifi.ap_pass);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool app_wifi_save(void)
{
    char *json = wifi_full_json();
    if (!json) return false;
    bool ok = nvs_save_string(NVS_NS_WIFI, NVS_KEY_WIFI, json);
    free(json);
    ESP_LOGI(TAG, "wifi saved (%s)", ok ? "ok" : "FAIL");
    return ok;
}

bool app_wifi_load(void)
{
    char *json = nvs_load_string(NVS_NS_WIFI, NVS_KEY_WIFI);
    if (!json) {
        ESP_LOGI(TAG, "no wifi credentials in NVS, using defaults");
        app_wifi_defaults();
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (root) {
        const cJSON *v;
        v = cJSON_GetObjectItem(root, "ssid");
        if (cJSON_IsString(v)) strncpy(g_wifi.ssid, v->valuestring, CFG_SSID_LEN - 1);
        v = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(v)) strncpy(g_wifi.pass, v->valuestring, CFG_PASS_LEN - 1);
        v = cJSON_GetObjectItem(root, "configured"); g_wifi.configured = cJSON_IsTrue(v);
        v = cJSON_GetObjectItem(root, "ap_ssid");
        if (cJSON_IsString(v)) strncpy(g_wifi.ap_ssid, v->valuestring, CFG_SSID_LEN - 1);
        v = cJSON_GetObjectItem(root, "ap_pass");
        if (cJSON_IsString(v)) strncpy(g_wifi.ap_pass, v->valuestring, CFG_PASS_LEN - 1);
        cJSON_Delete(root);
    }
    free(json);
    if (g_wifi.ap_ssid[0] == '\0') {
        strncpy(g_wifi.ap_ssid, "argimbss", CFG_SSID_LEN - 1);
    }
    ESP_LOGI(TAG, "wifi loaded from NVS (ssid=%s configured=%d)", g_wifi.ssid, g_wifi.configured);
    return true;
}

bool app_wifi_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    app_wifi_defaults();
    ESP_LOGI(TAG, "wifi credentials erased (%s)", err == ESP_OK ? "ok" : "FAIL");
    return err == ESP_OK;
}

// ---- Flag WiFi in NVS (macchina a stati AP/STA esclusivi) - dal gateway ----
#define NVS_KEY_CRED_VALIDATED  "cred_valid"
#define NVS_KEY_FORCE_AP        "force_ap"

static bool wifi_flag_get(const char *key, bool defval)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) return defval;
    uint8_t v = defval ? 1 : 0;
    esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    if (err != ESP_OK) return defval;
    return v != 0;
}

static bool wifi_flag_set(const char *key, bool val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, key, val ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool app_wifi_cred_validated_get(void)   { return wifi_flag_get(NVS_KEY_CRED_VALIDATED, false); }
bool app_wifi_cred_validated_set(bool v) { return wifi_flag_set(NVS_KEY_CRED_VALIDATED, v); }
bool app_wifi_force_ap_get(void)         { return wifi_flag_get(NVS_KEY_FORCE_AP, false); }
bool app_wifi_force_ap_set(bool v)       { return wifi_flag_set(NVS_KEY_FORCE_AP, v); }

// ============================================================================
//  VALIDAZIONE + IMPORT (import/export config, come il gateway)
// ============================================================================

bool is_allowed_baud(uint32_t baud)
{
    static const uint32_t allowed[] = RTU_ALLOWED_BAUDS;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == baud) return true;
    }
    return false;
}

// Valida l'INTERA config del simulatore. Ritorna NULL se ok, altrimenti stringa
// statica col primo problema.
const char *app_config_validate_all(const cfg_sim_t *cfg)
{
    if (!is_allowed_baud(cfg->serial.baudrate)) return "baudrate non ammesso";
    if (cfg->serial.data_bits != 7 && cfg->serial.data_bits != 8) return "data_bits deve essere 7 o 8";
    if (cfg->serial.stop_bits != 1 && cfg->serial.stop_bits != 2) return "stop_bits deve essere 1 o 2";
    if (cfg->serial.parity > 2) return "parity non valida (0=none/1=odd/2=even)";
    if (cfg->serial.modbus_id < 1 || cfg->serial.modbus_id > 247) return "unit_id fuori range (1..247)";
    return NULL;
}

const char *app_config_import(const char *json)
{
    if (!json) return "json mancante";
    if (!cfg_lock()) return "config occupata, riprova";

    // Parto da default puliti, parso e valido; se fallisce, ripristino da file/NVS.
    app_config_defaults();
    if (!app_config_from_json(json)) {
        app_config_load();
        cfg_unlock();
        return "config non valida";
    }
    const char *err = app_config_validate_all(&g_cfg);
    if (err) {
        app_config_load();
        cfg_unlock();
        return err;
    }
    if (!app_config_save()) {
        app_config_load();
        cfg_unlock();
        return "impossibile salvare la configurazione";
    }
    cfg_unlock();
    return NULL;
}

// ============================================================================
//  MUTEX CONFIG
// ============================================================================

void cfg_mutex_init(void)
{
    if (!s_cfg_mutex) {
        s_cfg_mutex = xSemaphoreCreateMutex();
        if (!s_cfg_mutex) ESP_LOGE(TAG, "config mutex creation FAILED");
    }
}

bool cfg_lock(void)
{
    if (!s_cfg_mutex) return false;
    if (xSemaphoreTake(s_cfg_mutex, pdMS_TO_TICKS(CFG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "cfg_lock TIMEOUT (%d ms)", CFG_LOCK_TIMEOUT_MS);
        return false;
    }
    return true;
}

void cfg_unlock(void)
{
    if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex);
}

// ============================================================================
//  INIT
// ============================================================================

void app_config_init(void)
{
    cfg_mutex_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_wifi_load();      // credenziali (o default)
    app_config_load();    // config simulatore (o default)
}
