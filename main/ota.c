// =============================================================================
//  ota.c - Aggiornamento firmware OTA (schema A/B con rollback)
// =============================================================================
#include "ota.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";

// Stato globale (una sola sessione OTA alla volta: un mutex la protegge).
struct ota_session {
    esp_ota_handle_t handle;
    const esp_partition_t *part;   // slot inattivo destinazione
    size_t written;                // byte scritti finora
    size_t total;                  // dimensione attesa (0 = unknown)
    bool   active;
};

static SemaphoreHandle_t s_lock;         // serializza le sessioni OTA
static ota_session_t     s_session;      // sessione corrente (una alla volta)
static volatile bool     s_running;      // true durante un OTA (per lo status)
static volatile size_t   s_prog;         // progress pubblicato per lo status
static volatile size_t   s_prog_total;   // total pubblicato per lo status

static void lock_init_once(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

// -----------------------------------------------------------------------------
// PUSH: begin / write / end / abort
// -----------------------------------------------------------------------------
ota_session_t *ota_begin(size_t total_size, char *out_err, size_t err_len)
{
    lock_init_once();
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        if (out_err) snprintf(out_err, err_len, "another OTA is already running");
        return NULL;
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        if (out_err) snprintf(out_err, err_len, "no OTA partition available");
        xSemaphoreGive(s_lock);
        return NULL;
    }

    esp_ota_handle_t h = 0;
    // OTA_SIZE_UNKNOWN fa erase progressivo; se conosciamo la size, e' piu' efficiente.
    esp_err_t err = esp_ota_begin(next, total_size ? total_size : OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
        if (out_err) snprintf(out_err, err_len, "esp_ota_begin failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return NULL;
    }

    s_session.handle  = h;
    s_session.part    = next;
    s_session.written = 0;
    s_session.total   = total_size;
    s_session.active  = true;
    s_running = true;
    s_prog = 0;
    s_prog_total = total_size;

    ESP_LOGI(TAG, "OTA started -> partition '%s' (size hint %u)",
             next->label, (unsigned)total_size);
    return &s_session;
}

esp_err_t ota_write(ota_session_t *s, const void *data, size_t len)
{
    if (!s || !s->active) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;

    esp_err_t err = esp_ota_write(s->handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed at %u: %s",
                 (unsigned)s->written, esp_err_to_name(err));
        return err;
    }
    s->written += len;
    s_prog = s->written;
    return ESP_OK;
}

esp_err_t ota_end(ota_session_t *s, char *out_err, size_t err_len)
{
    if (!s || !s->active) {
        if (out_err) snprintf(out_err, err_len, "no active OTA session");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(s->handle);   // valida checksum/magic dell'immagine
    if (err != ESP_OK) {
        if (out_err) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                snprintf(out_err, err_len, "image validation failed (corrupt or not an app)");
            else
                snprintf(out_err, err_len, "esp_ota_end failed: %s", esp_err_to_name(err));
        }
        s->active = false; s_running = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    err = esp_ota_set_boot_partition(s->part);
    if (err != ESP_OK) {
        if (out_err) snprintf(out_err, err_len, "set_boot_partition failed: %s", esp_err_to_name(err));
        s->active = false; s_running = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    ESP_LOGI(TAG, "OTA complete: %u bytes written to '%s', boot set. Reboot to apply.",
             (unsigned)s->written, s->part->label);
    s->active = false; s_running = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void ota_abort(ota_session_t *s)
{
    if (!s || !s->active) return;
    esp_ota_abort(s->handle);
    s->active = false;
    s_running = false;
    ESP_LOGW(TAG, "OTA aborted after %u bytes", (unsigned)s->written);
    xSemaphoreGive(s_lock);
}

size_t ota_written(const ota_session_t *s)
{
    return s ? s->written : 0;
}

// -----------------------------------------------------------------------------
// PULL: download da URL via esp_https_ota
// -----------------------------------------------------------------------------
esp_err_t ota_from_url(const char *url, char *out_err, size_t err_len)
{
    lock_init_once();
    if (!url || url[0] == '\0') {
        if (out_err) snprintf(out_err, err_len, "empty URL");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        if (out_err) snprintf(out_err, err_len, "another OTA is already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true; s_prog = 0; s_prog_total = 0;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        // Per HTTPS con CA note serve un bundle certificati; per URL http semplici
        // non serve. Se in futuro si scarica da https con CA custom, agganciare qui
        // .cert_pem o usare esp_crt_bundle_attach.
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "OTA from URL: %s", url);
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        if (out_err) snprintf(out_err, err_len, "https_ota_begin failed: %s", esp_err_to_name(err));
        s_running = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size > 0) s_prog_total = (size_t)image_size;

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        s_prog = (size_t)esp_https_ota_get_image_len_read(handle);
    }

    if (err != ESP_OK) {
        if (out_err) snprintf(out_err, err_len, "download/write failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        s_running = false;
        xSemaphoreGive(s_lock);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        if (out_err) snprintf(out_err, err_len, "incomplete image received");
        esp_https_ota_abort(handle);
        s_running = false;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle);   // valida e imposta boot partition
    s_running = false;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        if (out_err) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                snprintf(out_err, err_len, "image validation failed");
            else
                snprintf(out_err, err_len, "https_ota_finish failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "OTA from URL complete. Reboot to apply.");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Rollback / stato
// -----------------------------------------------------------------------------
void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        // Siamo appena partiti da un OTA e non ancora confermati: confermiamo ora.
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK)
            ESP_LOGI(TAG, "running image marked VALID (rollback cancelled)");
        else
            ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
    // Se non e' PENDING_VERIFY, non c'e' nulla da confermare: no-op sicuro.
}

int ota_status_json(char *out_json, size_t json_len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    bool pending = (state == ESP_OTA_IMG_PENDING_VERIFY);

    int n = snprintf(out_json, json_len,
        "{\"running\":%s,\"progress\":%u,\"total\":%u,"
        "\"app_version\":\"%s\",\"running_partition\":\"%s\",\"pending_verify\":%s}",
        s_running ? "true" : "false",
        (unsigned)s_prog, (unsigned)s_prog_total,
        desc ? desc->version : "?",
        running ? running->label : "?",
        pending ? "true" : "false");

    return (n > 0 && (size_t)n < json_len) ? n : -1;
}
