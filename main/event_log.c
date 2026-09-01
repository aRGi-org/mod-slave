// =============================================================================
//  event_log.c - Registro eventi (buffer circolare RAM + persistenza flash)
// =============================================================================
#include "event_log.h"
#include "fs_storage.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "event_log";

// File di persistenza su LittleFS.
#define EVLOG_FILE        "eventlog.json"
// Ogni quanti eventi INFO accumulati forzare una scrittura su flash (batch, per
// minimizzare il wear). Gli eventi WARN/ERROR vengono scritti subito.
#define EVLOG_BATCH_INFO  20
// Soglia ora valida (2020-01-01) per capire se il timestamp e' reale.
#define TIME_VALID_THRESHOLD  1577836800

// ---- stato interno ----
static evlog_entry_t s_buf[EVENT_LOG_CAPACITY];
static int  s_count = 0;        // numero eventi validi (max CAPACITY)
static int  s_head  = 0;        // indice della prossima scrittura (circolare)
static bool s_persist = false;  // persistenza su flash attiva
static int  s_dirty_info = 0;   // eventi INFO non ancora scritti su flash
static SemaphoreHandle_t s_mtx = NULL;

// ---------------------------------------------------------------------------
static inline void lock(void)   { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static inline void unlock(void) { if (s_mtx) xSemaphoreGive(s_mtx); }

// Serializza il buffer (dal piu' recente al piu' vecchio) in JSON. Chiamare con
// lock gia' preso.
static char *serialize_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    // Dal piu' recente: parto da head-1 e vado indietro per s_count elementi.
    for (int i = 0; i < s_count; i++) {
        int idx = (s_head - 1 - i + EVENT_LOG_CAPACITY) % EVENT_LOG_CAPACITY;
        evlog_entry_t *e = &s_buf[idx];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ts", (double)e->ts_unix);
        cJSON_AddNumberToObject(o, "up", (double)e->uptime_ms);
        cJSON_AddNumberToObject(o, "lv", (int)e->level);
        cJSON_AddStringToObject(o, "m", e->msg);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

// Scrive il buffer corrente su flash. Chiamare con lock gia' preso.
static void persist_locked(void)
{
    if (!s_persist) return;
    char *json = serialize_locked();
    if (!json) return;
    esp_err_t err = fs_write_text(EVLOG_FILE, json, strlen(json));
    free(json);
    if (err == ESP_OK) {
        s_dirty_info = 0;
    } else {
        ESP_LOGW(TAG, "persist to flash failed (%s)", esp_err_to_name(err));
    }
}

// Ricarica gli eventi salvati su flash nel buffer. Chiamare all'init.
static void reload_from_flash(void)
{
    if (!fs_exists(EVLOG_FILE)) return;
    // Leggo il file (riuso l'helper di lettura di fs_storage tramite fopen).
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", FS_BASE_PATH, EVLOG_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return; }

    // Il JSON e' dal piu' recente al piu' vecchio. Per ripopolare il buffer nello
    // stesso ordine cronologico, inserisco in ordine inverso (dal piu' vecchio).
    int n = cJSON_GetArraySize(arr);
    if (n > EVENT_LOG_CAPACITY) n = EVENT_LOG_CAPACITY;
    for (int i = n - 1; i >= 0; i--) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!o) continue;
        evlog_entry_t *e = &s_buf[s_head];
        cJSON *ts = cJSON_GetObjectItem(o, "ts");
        cJSON *up = cJSON_GetObjectItem(o, "up");
        cJSON *lv = cJSON_GetObjectItem(o, "lv");
        cJSON *m  = cJSON_GetObjectItem(o, "m");
        e->ts_unix   = ts ? (int64_t)ts->valuedouble : 0;
        e->uptime_ms = up ? (uint32_t)up->valuedouble : 0;
        e->level     = lv ? (evlog_level_t)lv->valueint : EVLOG_INFO;
        if (m && cJSON_IsString(m)) {
            strncpy(e->msg, m->valuestring, EVENT_LOG_MSG_LEN - 1);
            e->msg[EVENT_LOG_MSG_LEN - 1] = '\0';
        } else {
            e->msg[0] = '\0';
        }
        s_head = (s_head + 1) % EVENT_LOG_CAPACITY;
        if (s_count < EVENT_LOG_CAPACITY) s_count++;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "reloaded %d events from flash", s_count);
}

// ---------------------------------------------------------------------------
void event_log_init(bool persist)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    lock();
    s_count = 0; s_head = 0; s_dirty_info = 0;
    s_persist = persist;
    if (persist) reload_from_flash();
    unlock();
    ESP_LOGI(TAG, "event log ready (capacity %d, persist %s)",
             EVENT_LOG_CAPACITY, persist ? "on" : "off");
}

void event_log_set_persist(bool persist)
{
    lock();
    bool was = s_persist;
    s_persist = persist;
    if (persist && !was) {
        // Appena attivata: salva subito il buffer corrente come baseline.
        persist_locked();
    }
    unlock();
    ESP_LOGI(TAG, "event log persistence %s", persist ? "enabled" : "disabled");
}

void event_log(evlog_level_t level, const char *fmt, ...)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    evlog_entry_t e;
    time_t now = time(NULL);
    e.ts_unix   = (now >= TIME_VALID_THRESHOLD) ? (int64_t)now : 0;
    e.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    e.level     = level;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);

    lock();
    s_buf[s_head] = e;
    s_head = (s_head + 1) % EVENT_LOG_CAPACITY;
    if (s_count < EVENT_LOG_CAPACITY) s_count++;

    // Persistenza minimizzata: WARN/ERROR subito su flash; INFO accumulati e
    // scritti a batch (ogni EVLOG_BATCH_INFO) per ridurre il wear.
    if (s_persist) {
        if (level >= EVLOG_WARN) {
            persist_locked();
        } else if (++s_dirty_info >= EVLOG_BATCH_INFO) {
            persist_locked();
        }
    }
    unlock();

    // Eco anche sul log di sistema, cosi' l'evento compare anche sulla seriale.
    esp_log_level_t sl = (level == EVLOG_ERROR) ? ESP_LOG_ERROR
                       : (level == EVLOG_WARN)  ? ESP_LOG_WARN : ESP_LOG_INFO;
    ESP_LOG_LEVEL(sl, TAG, "%s", e.msg);
}

char *event_log_to_json(void)
{
    lock();
    char *out = serialize_locked();
    unlock();
    return out;
}

void event_log_flush(void)
{
    lock();
    persist_locked();
    unlock();
}
