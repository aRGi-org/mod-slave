#include "auth.h"
#include "board_config.h"

#include <string.h>
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

static const char *TAG = "auth";

#define NVS_NS_AUTH   "auth"
#define NVS_KEY_USERS "users"
#define AUTH_NUM_USERS 2   // admin + user

// Password iniziali di fabbrica.
#define ADMIN_INIT_PW  "nimda1965"
#define USER_INIT_PW   "resu1964"

static auth_user_t s_users[AUTH_NUM_USERS];

// ---- Hashing PBKDF2-HMAC-SHA256 ----
static bool pbkdf2(const char *password, const uint8_t *salt, size_t salt_len,
                   uint8_t *out_hash)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return false;

    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)password, strlen(password),
        salt, salt_len,
        AUTH_PBKDF2_ITERS,
        AUTH_HASH_LEN, out_hash);

    if (ret != 0) {
        ESP_LOGE(TAG, "pbkdf2 err -0x%04x", -ret);
        return false;
    }
    return true;
}

// Imposta username+password+ruolo su uno slot utente (genera nuovo salt).
static void set_user(auth_user_t *u, const char *username, const char *password,
                     auth_role_t role, bool must_change)
{
    memset(u, 0, sizeof(*u));
    strncpy(u->username, username, AUTH_USERNAME_LEN - 1);
    u->role = role;
    u->must_change = must_change;
    esp_fill_random(u->salt, AUTH_SALT_LEN);
    pbkdf2(password, u->salt, AUTH_SALT_LEN, u->hash);
}

// ---- Confronto costante nel tempo (evita timing attack) ----
static bool const_time_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// ---- API ----

void auth_reset_defaults(void)
{
    set_user(&s_users[0], "admin", ADMIN_INIT_PW, ROLE_ADMIN, true);
    set_user(&s_users[1], "user",  USER_INIT_PW,  ROLE_USER,  true);
    ESP_LOGI(TAG, "users reset to defaults (must_change=true)");
}

static auth_user_t *find_user(const char *username)
{
    for (int i = 0; i < AUTH_NUM_USERS; i++) {
        if (strncmp(s_users[i].username, username, AUTH_USERNAME_LEN) == 0) {
            return &s_users[i];
        }
    }
    return NULL;
}

auth_role_t auth_check(const char *username, const char *password, bool *out_must_change)
{
    auth_user_t *u = find_user(username);
    if (!u) return ROLE_NONE;

    uint8_t test[AUTH_HASH_LEN];
    if (!pbkdf2(password, u->salt, AUTH_SALT_LEN, test)) return ROLE_NONE;

    if (!const_time_eq(test, u->hash, AUTH_HASH_LEN)) return ROLE_NONE;

    if (out_must_change) *out_must_change = u->must_change;
    return u->role;
}

bool auth_change_password(const char *username, const char *old_pw, const char *new_pw)
{
    auth_user_t *u = find_user(username);
    if (!u) return false;

    // Verifica la vecchia password.
    uint8_t test[AUTH_HASH_LEN];
    if (!pbkdf2(old_pw, u->salt, AUTH_SALT_LEN, test)) return false;
    if (!const_time_eq(test, u->hash, AUTH_HASH_LEN)) return false;

    // Imposta la nuova (nuovo salt + hash), azzera must_change.
    esp_fill_random(u->salt, AUTH_SALT_LEN);
    if (!pbkdf2(new_pw, u->salt, AUTH_SALT_LEN, u->hash)) return false;
    u->must_change = false;

    auth_save();
    ESP_LOGI(TAG, "password changed for '%s'", username);
    return true;
}

// Admin reset: set a user's password WITHOUT knowing the old one.
// Caller must enforce that only an admin can invoke this.
bool auth_set_password(const char *username, const char *new_pw)
{
    auth_user_t *u = find_user(username);
    if (!u) return false;
    esp_fill_random(u->salt, AUTH_SALT_LEN);
    if (!pbkdf2(new_pw, u->salt, AUTH_SALT_LEN, u->hash)) return false;
    u->must_change = false;
    auth_save();
    ESP_LOGI(TAG, "password reset (admin) for '%s'", username);
    return true;
}

bool auth_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AUTH, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_USERS, s_users, sizeof(s_users));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool auth_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AUTH, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(s_users);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_USERS, s_users, &len);
    nvs_close(h);
    return (err == ESP_OK && len == sizeof(s_users));
}

bool auth_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_AUTH, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    // Ripristina i default in RAM (admin/user iniziali, must_change=true).
    auth_reset_defaults();
    ESP_LOGI(TAG, "users wiped and reset to defaults");
    return true;
}

void auth_init(void)
{
    if (auth_load()) {
        ESP_LOGI(TAG, "users loaded from NVS");
    } else {
        ESP_LOGI(TAG, "no users in NVS, applying defaults");
        auth_reset_defaults();
        auth_save();
    }
}