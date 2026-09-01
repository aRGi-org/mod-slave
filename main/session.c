#include "session.h"
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "session";

typedef struct {
    bool         used;
    char         token[SESSION_TOKEN_LEN];
    char         username[AUTH_USERNAME_LEN];
    auth_role_t  role;
    int64_t      last_activity_us;   // per il timeout di inattivita'
} session_t;

static session_t s_sessions[SESSION_MAX];

// Genera un token esadecimale random (128 bit di entropia).
static void gen_token(char *out)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0x0F];
    }
    out[32] = '\0';
}

static bool expired(const session_t *s, int64_t now_us)
{
    return (now_us - s->last_activity_us) > ((int64_t)SESSION_TIMEOUT_MS * 1000);
}

bool session_create(const char *username, auth_role_t role, char *out_token)
{
    int64_t now = esp_timer_get_time();

    // 1) Pulizia proattiva: libera TUTTI gli slot scaduti (anche quelli che
    //    nessuno ha piu' validato, che altrimenti resterebbero "used" per sempre).
    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].used && expired(&s_sessions[i], now)) {
            s_sessions[i].used = false;
            ESP_LOGI(TAG, "expired session reclaimed (slot %d)", i);
        }
    }

    // 2) Un utente = una sessione: se ha gia' uno slot valido, riusa quello
    //    (rigenera il token) invece di occuparne un altro. Evita l'accumulo
    //    che con SESSION_MAX piccolo esauriva gli slot ("no session slot").
    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].used &&
            strncmp(s_sessions[i].username, username, AUTH_USERNAME_LEN - 1) == 0) {
            gen_token(s_sessions[i].token);
            s_sessions[i].role = role;
            s_sessions[i].last_activity_us = now;
            strcpy(out_token, s_sessions[i].token);
            ESP_LOGI(TAG, "session reused for '%s' (slot %d)", username, i);
            return true;
        }
    }

    // 3) Slot libero.
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!s_sessions[i].used) {
            s_sessions[i].used = true;
            gen_token(s_sessions[i].token);
            strncpy(s_sessions[i].username, username, AUTH_USERNAME_LEN - 1);
            s_sessions[i].username[AUTH_USERNAME_LEN - 1] = '\0';
            s_sessions[i].role = role;
            s_sessions[i].last_activity_us = now;
            strcpy(out_token, s_sessions[i].token);
            ESP_LOGI(TAG, "session created for '%s' (slot %d)", username, i);
            return true;
        }
    }

    ESP_LOGW(TAG, "no free session slot");
    return false;
}

bool session_validate(const char *token, auth_role_t *out_role, char *out_user)
{
    if (!token || token[0] == '\0') return false;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].used &&
            strncmp(s_sessions[i].token, token, SESSION_TOKEN_LEN) == 0) {

            if (expired(&s_sessions[i], now)) {
                s_sessions[i].used = false;   // scaduta: libera lo slot
                ESP_LOGI(TAG, "session expired (slot %d)", i);
                return false;
            }
            // Valida: aggiorna l'attivita' (sliding timeout).
            s_sessions[i].last_activity_us = now;
            if (out_role) *out_role = s_sessions[i].role;
            if (out_user) {
                strncpy(out_user, s_sessions[i].username, AUTH_USERNAME_LEN);
            }
            return true;
        }
    }
    return false;
}

void session_destroy(const char *token)
{
    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].used &&
            strncmp(s_sessions[i].token, token, SESSION_TOKEN_LEN) == 0) {
            s_sessions[i].used = false;
            ESP_LOGI(TAG, "session closed (slot %d)", i);
            return;
        }
    }
}

void session_destroy_all(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    ESP_LOGI(TAG, "tutte le sessioni invalidate");
}