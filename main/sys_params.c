// =============================================================================
//  sys_params.c - Parametri di sistema (log-level per sottosistema)
// =============================================================================
#include "sys_params.h"

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "sysparams";

#define NVS_NS        "sysparams"
#define NVS_KEY_LOG   "log_levels"      // blob: uint8_t[LOG_SUBSYS_COUNT]

// Default di fabbrica: NONE per tutti (silenzio totale).
#define LOG_DEFAULT_LEVEL  ESP_LOG_NONE

// Stato corrente in RAM.
static uint8_t s_levels[LOG_SUBSYS_COUNT];

// ---- Mappa sottosistema -> lista di TAG (nostri + esp-modbus) ----
// L'ultimo elemento di ogni lista e' NULL (sentinella).
static const char *TAGS_RTU_COM1[]   = { "master_rtu", "mb_port.serial", NULL };
static const char *TAGS_RTU_COM2[]   = { "slave_rtu", NULL };
static const char *TAGS_MASTER_TCP[] = { "master_tcp", "mb_port.tcp.master", "mbc_tcp.master", "MB_CONTROLLER_MASTER", "mb_driver", "mb_object.master", "port.utils", NULL };
static const char *TAGS_SLAVE_TCP[]  = { "slave_tcp", "mb_port.tcp.slave", "mbc_tcp.slave", "MB_CONTROLLER_SLAVE", NULL };
static const char *TAGS_WEB[]        = { "web", "auth", "session", "httpd_uri", "httpd_txrx", NULL };
static const char *TAGS_WIFI[]       = { "wifi_mgr", "wifi", NULL };
static const char *TAGS_DIAG[]       = { "mb_diag", NULL };
static const char *TAGS_SYSTEM[]     = { "main", "main_task", "mb_hub", "app_config", "app_lang", "ota", "telemetry", NULL };

typedef struct {
    const char  *key;      // chiave testuale (stabile, per UI/NVS)
    const char  *name;     // nome leggibile
    const char **tags;     // lista TAG terminata da NULL
} subsys_info_t;

static const subsys_info_t SUBSYS[LOG_SUBSYS_COUNT] = {
    [LOG_SUBSYS_RTU_COM1]   = { "rtu_com1",   "RTU COM1",   TAGS_RTU_COM1   },
    [LOG_SUBSYS_RTU_COM2]   = { "rtu_com2",   "RTU COM2",   TAGS_RTU_COM2   },
    [LOG_SUBSYS_MASTER_TCP] = { "master_tcp", "Master TCP", TAGS_MASTER_TCP },
    [LOG_SUBSYS_SLAVE_TCP]  = { "slave_tcp",  "Slave TCP",  TAGS_SLAVE_TCP  },
    [LOG_SUBSYS_WEB]        = { "web",        "Web / Auth", TAGS_WEB        },
    [LOG_SUBSYS_WIFI]       = { "wifi",       "WiFi",       TAGS_WIFI       },
    [LOG_SUBSYS_DIAG]       = { "diag",       "Diagnostics",TAGS_DIAG       },
    [LOG_SUBSYS_SYSTEM]     = { "system",     "System",     TAGS_SYSTEM     },
};

const char *sys_log_subsys_key(log_subsys_t s)
{
    return (s < LOG_SUBSYS_COUNT) ? SUBSYS[s].key : "?";
}
const char *sys_log_subsys_name(log_subsys_t s)
{
    return (s < LOG_SUBSYS_COUNT) ? SUBSYS[s].name : "?";
}

// Applica il livello di un sottosistema a tutti i suoi TAG.
static void apply_subsys(log_subsys_t s)
{
    if (s >= LOG_SUBSYS_COUNT) return;
    esp_log_level_t lvl = (esp_log_level_t)s_levels[s];
    for (const char **t = SUBSYS[s].tags; *t; t++) {
        esp_log_level_set(*t, lvl);
    }
}

static void apply_all(void)
{
    for (int s = 0; s < LOG_SUBSYS_COUNT; s++) apply_subsys((log_subsys_t)s);
}

// ---- Persistenza NVS ----
static void load_from_nvs(void)
{
    // Default in RAM prima di tentare il load.
    for (int i = 0; i < LOG_SUBSYS_COUNT; i++) s_levels[i] = LOG_DEFAULT_LEVEL;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // mai salvato: default

    uint8_t buf[LOG_SUBSYS_COUNT];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LOG, buf, &len);
    if (err == ESP_OK && len == LOG_SUBSYS_COUNT) {
        for (int i = 0; i < LOG_SUBSYS_COUNT; i++) {
            s_levels[i] = (buf[i] <= ESP_LOG_VERBOSE) ? buf[i] : LOG_DEFAULT_LEVEL;
        }
    }
    nvs_close(h);
}

static void save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS to save log levels");
        return;
    }
    nvs_set_blob(h, NVS_KEY_LOG, s_levels, LOG_SUBSYS_COUNT);
    nvs_commit(h);
    nvs_close(h);
}

// ---- API ----
void sys_params_init(void)
{
    load_from_nvs();
    apply_all();
    ESP_LOGI(TAG, "log levels applied (defaults NONE unless configured)");
}

uint8_t sys_log_level_get(log_subsys_t s)
{
    return (s < LOG_SUBSYS_COUNT) ? s_levels[s] : ESP_LOG_NONE;
}

bool sys_log_level_set(log_subsys_t s, uint8_t level)
{
    if (s >= LOG_SUBSYS_COUNT || level > ESP_LOG_VERBOSE) return false;
    s_levels[s] = level;
    apply_subsys(s);     // effetto immediato a runtime
    save_to_nvs();       // persistente
    return true;
}

int sys_params_log_json(char *out, size_t out_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "log_levels");
    for (int s = 0; s < LOG_SUBSYS_COUNT; s++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key",  SUBSYS[s].key);
        cJSON_AddStringToObject(o, "name", SUBSYS[s].name);
        cJSON_AddNumberToObject(o, "level", s_levels[s]);
        cJSON_AddItemToArray(arr, o);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return -1;

    int n = (int)strlen(str);
    if ((size_t)n < out_len) {
        memcpy(out, str, n + 1);
    } else {
        n = -1;
    }
    free(str);
    return n;
}

bool sys_params_log_apply_json(const char *json)
{
    if (!json) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    cJSON *arr = cJSON_GetObjectItem(root, "log_levels");
    bool any = false;
    if (cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            cJSON *k = cJSON_GetObjectItem(it, "key");
            cJSON *l = cJSON_GetObjectItem(it, "level");
            if (!cJSON_IsString(k) || !cJSON_IsNumber(l)) continue;
            int lvl = l->valueint;
            if (lvl < 0 || lvl > ESP_LOG_VERBOSE) continue;
            // trova il sottosistema dalla chiave
            for (int s = 0; s < LOG_SUBSYS_COUNT; s++) {
                if (strcmp(k->valuestring, SUBSYS[s].key) == 0) {
                    s_levels[s] = (uint8_t)lvl;
                    apply_subsys((log_subsys_t)s);
                    any = true;
                    break;
                }
            }
        }
    }
    cJSON_Delete(root);
    if (any) save_to_nvs();
    return any;
}
