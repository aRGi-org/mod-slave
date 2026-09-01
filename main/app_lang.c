#include "app_lang.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "app_lang";
static const char *NVS_NS_LANG = "lang";
static const char *NVS_KEY     = "code";

// Lingue ammesse.
static const char *ALLOWED[] = { "en", "it", "fr", "es", "de" };
#define N_ALLOWED (sizeof(ALLOWED)/sizeof(ALLOWED[0]))

// Stato in RAM (sempre valido dopo init).
static char s_lang[LANG_CODE_LEN] = "en";

static bool is_allowed(const char *code)
{
    if (!code) return false;
    for (size_t i = 0; i < N_ALLOWED; i++) {
        if (strcmp(code, ALLOWED[i]) == 0) return true;
    }
    return false;
}

void app_lang_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LANG, NVS_READONLY, &h) == ESP_OK) {
        char buf[LANG_CODE_LEN] = {0};
        size_t len = sizeof(buf);
        if (nvs_get_str(h, NVS_KEY, buf, &len) == ESP_OK && is_allowed(buf)) {
            strncpy(s_lang, buf, LANG_CODE_LEN - 1);
            s_lang[LANG_CODE_LEN - 1] = '\0';
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "current language: %s", s_lang);
}

const char *app_lang_get(void)
{
    return s_lang;
}

bool app_lang_set(const char *code)
{
    if (!is_allowed(code)) {
        ESP_LOGW(TAG, "language not allowed: %s", code ? code : "(null)");
        return false;
    }
    strncpy(s_lang, code, LANG_CODE_LEN - 1);
    s_lang[LANG_CODE_LEN - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS_LANG, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, s_lang);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "language saved: %s", s_lang);
        return true;
    }
    ESP_LOGE(TAG, "failed to save language");
    return false;
}
