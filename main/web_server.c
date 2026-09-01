#include "web_server.h"
#include "app_config.h"
#include "wifi_mgr.h"

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "fs_storage.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "json_utils.h"
#include "app_lang.h"

#include "auth.h"
#include "session.h"
#include "ota.h"
#include "sys_params.h"
#include "dashboard.h"
#include "net_time.h"
#include "event_log.h"
#include "app_version.h"
#include "esp_app_desc.h"

#include "lwip/sockets.h"
#include "board_config.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
//  Web server del SIMULATORE SLAVE RTU (fork ridotto del web_server del gateway).
//
//  TENUTO verbatim dal gateway: infrastruttura (login/logout/password, wifi,
//  ota, ntp, sysparams log, dashboard reg/bit, event log, config export/import,
//  language, reboot, status/appstate) + HTTPS su LittleFS.
//  RIMOSSO: device (/api/devices,/api/device), blocchi (/api/block),
//  master_tcp, slave_tcp, e la diagnostica mb_diag (/api/diag).
//  ADATTATO: /api/port (una porta: seriale + modalita' fisica, no ruoli).
//  AGGIUNTO: /api/datagen (modalita' generatore dati: static/anim + anim_ms).
// ============================================================================

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;

// ---------------------------------------------------------------------------
//  Helper comuni (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, char *json)
{
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json null");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static bool get_session_token(httpd_req_t *req, char *token, size_t token_size)
{
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }
    char *p = strstr(cookie, "SID=");
    if (!p) return false;
    p += 4;
    size_t i = 0;
    while (*p && *p != ';' && *p != ' ' && i < token_size - 1) {
        token[i++] = *p++;
    }
    token[i] = '\0';
    return i > 0;
}

static auth_role_t require_auth(httpd_req_t *req, auth_role_t min_role)
{
    char token[SESSION_TOKEN_LEN] = {0};
    auth_role_t role = ROLE_NONE;

    if (get_session_token(req, token, sizeof(token)) &&
        session_validate(token, &role, NULL)) {
        if (role >= min_role) {
            return role;
        }
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"forbidden\"}");
        return ROLE_NONE;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ROLE_NONE;
}

typedef enum {
    BODY_OK = 0,
    BODY_EMPTY,
    BODY_TOO_LARGE,
    BODY_NO_MEM,
    BODY_RECV_ERR
} body_result_t;

static char *read_body_ex(httpd_req_t *req, size_t max_len, body_result_t *out_res)
{
    size_t len = req->content_len;
    if (len == 0)        { if (out_res) *out_res = BODY_EMPTY;     return NULL; }
    if (len > max_len)   { if (out_res) *out_res = BODY_TOO_LARGE; return NULL; }
    char *buf = malloc(len + 1);
    if (!buf)            { if (out_res) *out_res = BODY_NO_MEM;    return NULL; }
    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) { free(buf); if (out_res) *out_res = BODY_RECV_ERR; return NULL; }
        received += r;
    }
    buf[len] = '\0';
    if (out_res) *out_res = BODY_OK;
    return buf;
}

static char *read_body(httpd_req_t *req, size_t max_len)
{
    return read_body_ex(req, max_len, NULL);
}

// ---------------------------------------------------------------------------
//  Stato: appstate (footer) + status
// ---------------------------------------------------------------------------

static esp_err_t appstate_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "fw_version", APP_VERSION_STRING);
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "heap_min",  (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    static int    s_cpu_load_cached = -1;
    static int64_t s_cpu_load_at_us = 0;
    const int64_t CPU_LOAD_CACHE_MS = 30000;
    int cpu_load = s_cpu_load_cached;
    int64_t now_us = esp_timer_get_time();
    if (s_cpu_load_at_us == 0 ||
        (now_us - s_cpu_load_at_us) >= (CPU_LOAD_CACHE_MS * 1000)) {
#if (configGENERATE_RUN_TIME_STATS == 1) && (configUSE_TRACE_FACILITY == 1)
        {
            UBaseType_t n = uxTaskGetNumberOfTasks();
            TaskStatus_t *arr = malloc(n * sizeof(TaskStatus_t));
            if (arr) {
                uint32_t total;
                n = uxTaskGetSystemState(arr, n, &total);
                if (total > 0) {
                    uint32_t idle = 0;
                    for (UBaseType_t i = 0; i < n; i++) {
                        if (strncmp(arr[i].pcTaskName, "IDLE", 4) == 0)
                            idle += arr[i].ulRunTimeCounter;
                    }
                    cpu_load = 100 - (int)((idle * 100ULL) / total);
                    if (cpu_load < 0) cpu_load = 0;
                    if (cpu_load > 100) cpu_load = 100;
                }
                free(arr);
            }
        }
#endif
        s_cpu_load_cached = cpu_load;
        s_cpu_load_at_us  = now_us;
    }
    cJSON_AddNumberToObject(o, "cpu_load", cpu_load);
    cJSON_AddBoolToObject(o, "sta_connected", wifi_mgr_sta_connected());

    {
        char iso[24];
        bool synced = net_time_utc_iso8601(iso, sizeof(iso));
        cJSON_AddBoolToObject(o, "time_synced", synced);
        if (synced) {
            cJSON_AddStringToObject(o, "utc", iso);
            char local[20];
            if (net_time_local_hhmm(local, sizeof(local))) {
                cJSON_AddStringToObject(o, "local_time", local);
            }
        }
        cJSON_AddStringToObject(o, "tz", g_cfg.ntp.posix_tz);
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

// GET /api/status -> stato runtime del simulatore.
static esp_err_t status_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "sta_connected", wifi_mgr_sta_connected());
    cJSON_AddBoolToObject(o, "ap_on", wifi_mgr_ap_is_on());

    char ip_str[16] = "0.0.0.0";
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (sta && wifi_mgr_sta_connected()) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
        }
    }
    cJSON_AddStringToObject(o, "sta_ip", ip_str);
    cJSON_AddStringToObject(o, "ap_ssid", g_wifi.ap_ssid);

    // Stato specifico del simulatore: ruolo fisso slave, id, phys, datagen.
    cJSON_AddStringToObject(o, "role", "slave_rtu");
    cJSON_AddNumberToObject(o, "modbus_id", g_cfg.serial.modbus_id);
    cJSON_AddNumberToObject(o, "baudrate", g_cfg.serial.baudrate);
    cJSON_AddStringToObject(o, "phys_mode", phys_mode_str(g_cfg.phys_mode));
    cJSON_AddStringToObject(o, "datagen", datagen_str(g_cfg.datagen));

    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

// GET /api/config -> intera configurazione del simulatore.
static esp_err_t config_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    return send_json(req, app_config_to_json());
}

// ---------------------------------------------------------------------------
//  Login / logout / password (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t login_post(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }

    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(in, "username"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(in, "password"));

    bool must_change = false;
    auth_role_t role = ROLE_NONE;
    if (user && pass) {
        role = auth_check(user, pass, &must_change);
    }

    if (role == ROLE_NONE) {
        event_log(EVLOG_WARN, "login failed for user '%s'", user ? user : "?");
        cJSON_Delete(in);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid credentials\"}");
        return ESP_OK;
    }

    char token[SESSION_TOKEN_LEN];
    if (!session_create(user, role, token)) {
        cJSON_Delete(in);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no session slot");
        return ESP_OK;
    }
    event_log(EVLOG_INFO, "login ok: user '%s' (role %d)", user, (int)role);

    char cookie[80];
    snprintf(cookie, sizeof(cookie), "SID=%s; Path=/; HttpOnly; Max-Age=%d",
             token, (SESSION_TIMEOUT_MS / 1000));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "role", role);
    cJSON_AddBoolToObject(out, "must_change", must_change);
    cJSON_AddStringToObject(out, "username", user);
    char *resp = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(in);
    return send_json(req, resp);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    char token[SESSION_TOKEN_LEN] = {0};
    if (get_session_token(req, token, sizeof(token))) {
        session_destroy(token);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "SID=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const char *validate_password_strength(const char *pw)
{
    if (!pw) return "password missing";
    if (strlen(pw) < 8) return "minimo 8 caratteri";

    bool has_upper = false, has_digit = false, has_special = false, has_lower = false;
    for (const char *p = pw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z')      has_upper = true;
        else if (c >= 'a' && c <= 'z') has_lower = true;
        else if (c >= '0' && c <= '9') has_digit = true;
        else if (c > 32 && c < 127)    has_special = true;
    }

    if (!has_upper)   return "serve almeno una maiuscola";
    if (!has_digit)   return "serve almeno un numero";
    if (!has_special) return "serve almeno un carattere speciale";
    (void)has_lower;
    return NULL;
}

static esp_err_t change_password_post(httpd_req_t *req)
{
    char token[SESSION_TOKEN_LEN] = {0};
    char username[AUTH_USERNAME_LEN] = {0};
    auth_role_t role = ROLE_NONE;
    if (!get_session_token(req, token, sizeof(token)) ||
        !session_validate(token, &role, username)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return ESP_OK;
    }

    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    const char *old_pw = cJSON_GetStringValue(cJSON_GetObjectItem(in, "old_password"));
    const char *new_pw = cJSON_GetStringValue(cJSON_GetObjectItem(in, "new_password"));

    if (!old_pw) {
        cJSON_Delete(in);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "old_password missing");
        return ESP_OK;
    }
    const char *pw_err = validate_password_strength(new_pw);
    if (pw_err) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", pw_err);
        char *estr = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(in);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, estr);
        free(estr);
        return ESP_OK;
    }

    bool ok = auth_change_password(username, old_pw, new_pw);
    cJSON_Delete(in);

    if (!ok) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"wrong old password\"}");
        return ESP_OK;
    }

    session_destroy_all();
    httpd_resp_set_hdr(req, "Set-Cookie", "SID=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t user_password_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    const char *username = cJSON_GetStringValue(cJSON_GetObjectItem(in, "username"));
    const char *new_pw   = cJSON_GetStringValue(cJSON_GetObjectItem(in, "new_password"));
    if (!username) { cJSON_Delete(in); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username missing"); return ESP_OK; }

    const char *pw_err = validate_password_strength(new_pw);
    if (pw_err) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", pw_err);
        char *estr = cJSON_PrintUnformatted(e);
        cJSON_Delete(e); cJSON_Delete(in);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, estr); free(estr);
        return ESP_OK;
    }

    bool ok = auth_set_password(username, new_pw);
    cJSON_Delete(in);
    if (!ok) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"user not found\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  WiFi (verbatim dal gateway)
// ---------------------------------------------------------------------------

static bool request_from_ap(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return false;

    struct sockaddr_in6 addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) < 0) {
        return false;
    }

    uint8_t *b = addr.sin6_addr.s6_addr;
    uint32_t client_ip = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                         ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];

    esp_ip4_addr_t ap_ip, ap_mask;
    esp_netif_str_to_ip4(AP_IP_ADDR, &ap_ip);
    esp_netif_str_to_ip4(AP_NETMASK, &ap_mask);
    uint32_t ap_ip_h   = esp_ip4_addr_get_byte(&ap_ip, 0) << 24 |
                         esp_ip4_addr_get_byte(&ap_ip, 1) << 16 |
                         esp_ip4_addr_get_byte(&ap_ip, 2) << 8  |
                         esp_ip4_addr_get_byte(&ap_ip, 3);
    uint32_t ap_mask_h = esp_ip4_addr_get_byte(&ap_mask, 0) << 24 |
                         esp_ip4_addr_get_byte(&ap_mask, 1) << 16 |
                         esp_ip4_addr_get_byte(&ap_mask, 2) << 8  |
                         esp_ip4_addr_get_byte(&ap_mask, 3);

    return (client_ip & ap_mask_h) == (ap_ip_h & ap_mask_h);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *base = app_wifi_to_json();
    if (!base) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_OK; }
    cJSON *o = cJSON_Parse(base);
    free(base);
    if (!o) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_OK; }
    cJSON_AddBoolToObject(o, "ap_mode", wifi_mgr_ap_is_on());
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

#define WIFI_SCAN_MAX 20
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    wifi_scan_entry_t *nets = calloc(WIFI_SCAN_MAX, sizeof(wifi_scan_entry_t));
    if (!nets) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_OK; }
    int n = wifi_mgr_scan(nets, WIFI_SCAN_MAX);
    if (n < 0) {
        free(nets);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "nets");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", nets[i].rssi);
        cJSON_AddNumberToObject(e, "channel", nets[i].channel);
        cJSON_AddBoolToObject(e, "open", nets[i].auth_open ? true : false);
        cJSON_AddItemToArray(arr, e);
    }
    free(nets);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return send_json(req, out);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    if (!request_from_ap(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"modifica WiFi consentita solo in modalita' AP\"}");
        return ESP_OK;
    }

    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }

    if (!app_wifi_from_json(body)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_OK;
    }
    free(body);

    if (!app_wifi_save()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }

    app_wifi_cred_validated_set(false);
    app_wifi_force_ap_set(false);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"credenziali salvate, riavvio in corso\"}");

    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Porta seriale del simulatore: seriale + modalita' fisica (NO ruoli).
//  Sostituisce port_get/port_put del gateway (che erano a ruoli e 2 porte).
// ---------------------------------------------------------------------------

// GET /api/port -> parametri seriali + modalita' fisica dell'unica porta.
static esp_err_t port_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;

    const cfg_serial_t *s = &g_cfg.serial;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", "slave");        // fisso: informativo per la UI
    cJSON_AddStringToObject(o, "phys_mode", phys_mode_str(g_cfg.phys_mode));
    cJSON_AddNumberToObject(o, "baudrate", s->baudrate);
    cJSON_AddNumberToObject(o, "parity", s->parity);
    cJSON_AddNumberToObject(o, "data_bits", s->data_bits);
    cJSON_AddNumberToObject(o, "stop_bits", s->stop_bits);
    cJSON_AddNumberToObject(o, "modbus_id", s->modbus_id);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

// PUT /api/port -> imposta seriale + modalita' fisica (admin). Cambiare questi
// parametri richiede un reboot per ricreare il controller RTU con la nuova UART.
static esp_err_t port_put(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *body = read_body(req, 512);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    // Copia di lavoro sui campi seriali (validazione prima di applicare).
    cfg_serial_t tmp = g_cfg.serial;
    port_phys_mode_t phys = g_cfg.phys_mode;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "baudrate"))  && cJSON_IsNumber(v)) tmp.baudrate  = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "parity"))    && cJSON_IsNumber(v)) tmp.parity    = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "data_bits")) && cJSON_IsNumber(v)) tmp.data_bits = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "stop_bits")) && cJSON_IsNumber(v)) tmp.stop_bits = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "modbus_id")) && cJSON_IsNumber(v)) tmp.modbus_id = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "phys_mode")) && cJSON_IsString(v)) {
        const char *ps = v->valuestring;
        if      (!strcmp(ps, "rs232")) phys = PHYS_RS232;
        else if (!strcmp(ps, "rs485")) phys = PHYS_RS485;
        else if (!strcmp(ps, "rs422")) phys = PHYS_RS422;
        else                            phys = PHYS_UART_TTL;
    }
    cJSON_Delete(in);

    // Validazione (riusa i limiti di app_config).
    if (!is_allowed_baud(tmp.baudrate))               { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "baudrate non ammesso"); return ESP_OK; }
    if (tmp.data_bits != 7 && tmp.data_bits != 8)     { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "data_bits 7 o 8"); return ESP_OK; }
    if (tmp.stop_bits != 1 && tmp.stop_bits != 2)     { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "stop_bits 1 o 2"); return ESP_OK; }
    if (tmp.parity > 2)                               { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parity 0/1/2"); return ESP_OK; }
    if (tmp.modbus_id < 1 || tmp.modbus_id > 247)     { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unit_id 1..247"); return ESP_OK; }

    if (!cfg_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"config busy, retry\"}");
        return ESP_OK;
    }
    g_cfg.serial    = tmp;
    g_cfg.phys_mode = phys;
    app_config_save();
    cfg_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"riavviare per applicare\"}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Generatore dati: /api/datagen (NUOVO). Modalita' static/anim + anim_ms.
// ---------------------------------------------------------------------------

static esp_err_t datagen_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", datagen_str(g_cfg.datagen));
    cJSON_AddNumberToObject(o, "anim_ms", g_cfg.anim_ms);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

// PUT /api/datagen -> { "mode":"static|anim", "anim_ms":N }. Admin.
// Il cambio modalita' ha effetto al reboot (il task generatore parte a boot).
static esp_err_t datagen_put(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    data_gen_mode_t mode = g_cfg.datagen;
    uint32_t anim_ms = g_cfg.anim_ms;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "mode")) && cJSON_IsString(v))
        mode = (!strcmp(v->valuestring, "anim")) ? DATAGEN_ANIM : DATAGEN_STATIC;
    if ((v = cJSON_GetObjectItem(in, "anim_ms")) && cJSON_IsNumber(v))
        anim_ms = (uint32_t)v->valuedouble;
    cJSON_Delete(in);

    if (anim_ms < 20 || anim_ms > 60000) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "anim_ms 20..60000"); return ESP_OK; }

    if (!cfg_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"config busy, retry\"}");
        return ESP_OK;
    }
    g_cfg.datagen = mode;
    g_cfg.anim_ms = anim_ms;
    app_config_save();
    cfg_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"riavviare per applicare\"}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Protocollo web HTTP/HTTPS: /api/websec (verbatim dal gateway)
// ---------------------------------------------------------------------------

// GET /api/websec -> stato protocollo web. Solo admin.
//  want_https   = protocollo richiesto in config (true=HTTPS)
//  active_https = protocollo realmente attivo ora (differisce se fallback a HTTP)
static esp_err_t websec_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "want_https", g_cfg.web_https);
    cJSON_AddBoolToObject(o, "active_https", web_server_is_https());
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

// PUT /api/websec { "https": true|false } -> imposta il protocollo web. Solo
// admin. Applicato al REBOOT (il server e' gia' avviato). Salva la config.
static esp_err_t websec_put(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;
    char body[128];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_OK; }
    body[n] = '\0';
    cJSON *in = cJSON_Parse(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    bool https_req;
    if (json_get_bool(in, "https", false, &https_req) != JGET_OK) {
        cJSON_Delete(in);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "https flag");
        return ESP_OK;
    }
    cJSON_Delete(in);

    if (!cfg_lock()) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy"); return ESP_OK; }
    g_cfg.web_https = https_req;
    app_config_save();
    cfg_unlock();

    event_log(EVLOG_INFO, "web protocol set to %s (reboot to apply)",
              https_req ? "HTTPS" : "HTTP");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"riavviare per applicare\"}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  NTP (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t ntp_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", g_cfg.ntp.enabled);
    cJSON_AddStringToObject(o, "server", g_cfg.ntp.server);
    cJSON_AddStringToObject(o, "tz", g_cfg.ntp.posix_tz);
    cJSON_AddStringToObject(o, "tz_name", g_cfg.ntp.tz_name);
    cJSON_AddBoolToObject(o, "synced", net_time_is_synced());
    cJSON_AddBoolToObject(o, "log_persist", g_cfg.log_persist);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

static esp_err_t ntp_put(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *body = read_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    bool enabled;
    char server[CFG_NTP_SERVER_LEN];
    char tz[CFG_NTP_TZ_LEN];
    char tz_name[CFG_NTP_TZNAME_LEN];
    bool log_persist;
    jget_result_t jr;
    bool ok = true;
    jr = json_get_bool(in, "enabled", true, &enabled);                    if (jr != JGET_OK) ok = false;
    jr = json_get_str (in, "server", server, CFG_NTP_SERVER_LEN, false);  if (jr != JGET_OK) ok = false;
    jr = json_get_str (in, "tz", tz, CFG_NTP_TZ_LEN, false);              if (jr != JGET_OK) ok = false;
    jr = json_get_str (in, "tz_name", tz_name, CFG_NTP_TZNAME_LEN, false);if (jr != JGET_OK) ok = false;
    jr = json_get_bool(in, "log_persist", false, &log_persist);           if (jr != JGET_OK) ok = false;
    cJSON_Delete(in);
    if (!ok) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid fields"); return ESP_OK; }

    if (!cfg_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"config busy, retry\"}");
        return ESP_OK;
    }
    g_cfg.ntp.enabled = enabled;
    if (server[0] != '\0') {
        strncpy(g_cfg.ntp.server, server, CFG_NTP_SERVER_LEN - 1);
        g_cfg.ntp.server[CFG_NTP_SERVER_LEN - 1] = '\0';
    }
    if (tz[0] != '\0') {
        strncpy(g_cfg.ntp.posix_tz, tz, CFG_NTP_TZ_LEN - 1);
        g_cfg.ntp.posix_tz[CFG_NTP_TZ_LEN - 1] = '\0';
    }
    if (tz_name[0] != '\0') {
        strncpy(g_cfg.ntp.tz_name, tz_name, CFG_NTP_TZNAME_LEN - 1);
        g_cfg.ntp.tz_name[CFG_NTP_TZNAME_LEN - 1] = '\0';
    }
    g_cfg.log_persist = log_persist;
    app_config_save();
    cfg_unlock();

    event_log_set_persist(g_cfg.log_persist);
    net_time_start(g_cfg.ntp.enabled, g_cfg.ntp.server, g_cfg.ntp.posix_tz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Config import/export (verbatim dal gateway, filename adattato)
// ---------------------------------------------------------------------------

static esp_err_t config_export_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;

    char *json = app_config_to_json();
    if (!json) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "export"); return ESP_OK; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"argimbss-config.json\"");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t config_import_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    body_result_t br;
    char *body = read_body_ex(req, 32768, &br);
    if (!body) {
        const char *msg;
        switch (br) {
            case BODY_EMPTY:     msg = "body vuoto";                       break;
            case BODY_TOO_LARGE: msg = "config troppo grande (max 32 KB)"; break;
            case BODY_NO_MEM:    msg = "memoria insufficiente";            break;
            default:             msg = "errore ricezione body";           break;
        }
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }

    const char *err = app_config_import(body);
    free(body);

    if (err) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", err);
        char *estr = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, estr); free(estr);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  OTA (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t ota_upload_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    int total = req->content_len;
    char errbuf[80] = {0};

    ota_session_t *s = ota_begin(total > 0 ? (size_t)total : 0, errbuf, sizeof(errbuf));
    if (!s) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errbuf[0] ? errbuf : "ota_begin failed");
        return ESP_OK;
    }

    char *buf = malloc(4096);
    if (!buf) {
        ota_abort(s);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem for chunk buffer");
        return ESP_OK;
    }

    int remaining = (total > 0) ? total : INT32_MAX;
    int received_total = 0;
    esp_err_t werr = ESP_OK;

    while (remaining > 0) {
        int want = remaining < 4096 ? remaining : 4096;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r < 0) { werr = ESP_FAIL; snprintf(errbuf, sizeof(errbuf), "recv error %d", r); break; }
        if (r == 0) break;

        werr = ota_write(s, buf, r);
        if (werr != ESP_OK) { snprintf(errbuf, sizeof(errbuf), "flash write failed at %d", received_total); break; }

        received_total += r;
        if (total > 0) remaining -= r;
    }
    free(buf);

    if (werr != ESP_OK) {
        ota_abort(s);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errbuf[0] ? errbuf : "ota write failed");
        return ESP_OK;
    }
    if (received_total == 0) {
        ota_abort(s);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty firmware");
        return ESP_OK;
    }

    if (ota_end(s, errbuf, sizeof(errbuf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errbuf[0] ? errbuf : "ota finalize failed");
        return ESP_OK;
    }

    ESP_LOGI("ota", "upload complete: %d bytes, rebooting in 1s", received_total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_url_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    body_result_t br;
    char *body = read_body_ex(req, 1024, &br);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vuoto o troppo grande"); return ESP_OK; }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json non valido"); return ESP_OK; }
    cJSON *u = cJSON_GetObjectItem(j, "url");
    if (!cJSON_IsString(u) || !u->valuestring[0]) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "campo 'url' mancante");
        return ESP_OK;
    }

    char errbuf[96] = {0};
    esp_err_t err = ota_from_url(u->valuestring, errbuf, sizeof(errbuf));
    cJSON_Delete(j);

    if (err != ESP_OK) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", errbuf[0] ? errbuf : "ota from url failed");
        char *estr = cJSON_PrintUnformatted(e); cJSON_Delete(e);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, estr); free(estr);
        return ESP_OK;
    }

    ESP_LOGI("ota", "url update complete, rebooting in 1s");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_status_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char buf[256];
    if (ota_status_json(buf, sizeof(buf)) < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status error");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Sysparams log levels (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t sysparams_log_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char buf[768];
    if (sys_params_log_json(buf, sizeof(buf)) < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "log params error");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t sysparams_log_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    body_result_t br;
    char *body = read_body_ex(req, 1024, &br);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vuoto o troppo grande"); return ESP_OK; }
    bool ok = sys_params_log_apply_json(body);
    free(body);

    if (!ok) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no valid log level in body"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Dashboard registri/bit (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t dashboard_regs_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;

    char query[96], val[16];
    int area = 0, start = 0, count = 64;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "area", val, sizeof(val)) == ESP_OK)
            area = (strcmp(val, "input") == 0) ? 1 : 0;
        if (httpd_query_key_value(query, "start", val, sizeof(val)) == ESP_OK)
            start = atoi(val);
        if (httpd_query_key_value(query, "count", val, sizeof(val)) == ESP_OK)
            count = atoi(val);
    }

    char *buf = malloc(2560);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_OK; }
    if (dashboard_regs_json(area, start, count, buf, 2560) < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "regs error");
        free(buf);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t dashboard_bits_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;

    char query[64], val[16];
    int area = 2;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "area", val, sizeof(val)) == ESP_OK)
            area = (strcmp(val, "discrete") == 0) ? 3 : 2;
    }

    char *buf = malloc(2560);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_OK; }
    if (dashboard_bits_json(area, buf, 2560) < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bits error");
        free(buf);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Event log (verbatim dal gateway)
// ---------------------------------------------------------------------------

static esp_err_t eventlog_get(httpd_req_t *req)
{
    if (require_auth(req, ROLE_USER) == ROLE_NONE) return ESP_OK;
    char *json = event_log_to_json();
    if (!json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    return send_json(req, json);
}

// ---------------------------------------------------------------------------
//  SPA + lingua + reboot (verbatim dal gateway)
// ---------------------------------------------------------------------------

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

static esp_err_t language_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "lang", app_lang_get());
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return send_json(req, out);
}

static esp_err_t language_put(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;

    char *body = read_body(req, 64);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(in, "lang"));
    bool ok = app_lang_set(code);
    cJSON_Delete(in);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"lingua non ammessa\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (require_auth(req, ROLE_ADMIN) == ROLE_NONE) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Registrazione handler + avvio HTTPS
// ---------------------------------------------------------------------------

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = method, .handler = h, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &u);
}

// Protocollo effettivamente attivo dopo l'avvio (puo' differire dal richiesto se
// HTTPS e' fallito ed e' scattato il fallback su HTTP). Letto da mDNS e dall'API.
static bool s_https_active = false;
bool web_server_is_https(void) { return s_https_active; }

// Avvia il server in HTTPS. Ritorna true se partito, false se cert mancanti o
// start fallito (il chiamante fa fallback su HTTP). cert.pem/key.pem sono
// precaricati nell'immagine LittleFS (sopravvivono agli OTA), self-signed.
static bool web_server_start_https(void)
{
    static char cert_buf[2560];
    static char key_buf[2560];
    int cert_len = fs_read_text("cert.pem", cert_buf, sizeof(cert_buf));
    int key_len  = fs_read_text("key.pem",  key_buf,  sizeof(key_buf));
    if (cert_len <= 0 || key_len <= 0) {
        ESP_LOGW(TAG, "HTTPS requested but cert.pem/key.pem missing -> fallback HTTP");
        return false;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.servercert     = (const uint8_t *)cert_buf;
    config.servercert_len = cert_len + 1;
    config.prvtkey_pem    = (const uint8_t *)key_buf;
    config.prvtkey_len    = key_len + 1;
    config.port_secure = 443;
    config.port_insecure = 0;      // in HTTPS la porta 80 non viene aperta
    config.httpd.max_uri_handlers = 48;
    config.httpd.lru_purge_enable = true;
    config.httpd.max_open_sockets = 2;   // TLS costa RAM: poche connessioni
    config.httpd.stack_size = 10240;
    config.httpd.recv_wait_timeout = 4;
    config.httpd.send_wait_timeout = 4;

    if (httpd_ssl_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS server start failed -> fallback HTTP");
        return false;
    }
    s_https_active = true;
    ESP_LOGI(TAG, "HTTPS server started on port 443");
    return true;
}

// Avvia il server in HTTP puro (porta 80). Ritorna true se partito.
static bool web_server_start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 48;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;   // senza TLS ogni connessione costa poco
    config.stack_size       = 8192;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return false;
    }
    s_https_active = false;
    ESP_LOGI(TAG, "HTTP server started on port 80");
    return true;
}

void web_server_start(void)
{
    // Sceglie il protocollo dalla config. Default HTTP (RAM tranquilla). Se HTTPS
    // e' richiesto ma non parte (cert mancanti / RAM), FALLBACK automatico su
    // HTTP: il device non resta mai senza interfaccia web (no lock-out).
    bool started = false;
    if (g_cfg.web_https) {
        started = web_server_start_https();
        if (!started) {
            event_log(EVLOG_WARN, "HTTPS non avviato, fallback su HTTP");
            started = web_server_start_http();
        }
    } else {
        started = web_server_start_http();
    }
    if (!started) {
        ESP_LOGE(TAG, "web server NOT started (both HTTPS and HTTP failed)");
        return;
    }

    // SPA + base
    register_uri("/",                    HTTP_GET,    root_get);
    register_uri("/api/language",        HTTP_GET,    language_get);
    register_uri("/api/language",        HTTP_PUT,    language_put);
    register_uri("/api/reboot",          HTTP_POST,   reboot_post);
    register_uri("/api/status",          HTTP_GET,    status_get);
    register_uri("/api/appstate",        HTTP_GET,    appstate_get);
    register_uri("/api/config",          HTTP_GET,    config_get);

    // Auth
    register_uri("/api/login",           HTTP_POST,   login_post);
    register_uri("/api/logout",          HTTP_POST,   logout_post);
    register_uri("/api/change_password", HTTP_POST,   change_password_post);
    register_uri("/api/user_password",   HTTP_POST,   user_password_post);

    // WiFi
    register_uri("/api/wifi",            HTTP_GET,    wifi_get);
    register_uri("/api/wifi/scan",       HTTP_GET,    wifi_scan_get);
    register_uri("/api/wifi",            HTTP_POST,   wifi_post);

    // Porta seriale + generatore dati (specifici del simulatore)
    register_uri("/api/port",            HTTP_GET,    port_get);
    register_uri("/api/port",            HTTP_PUT,    port_put);
    register_uri("/api/datagen",         HTTP_GET,    datagen_get);
    register_uri("/api/datagen",         HTTP_PUT,    datagen_put);

    // Protocollo web HTTP/HTTPS
    register_uri("/api/websec",          HTTP_GET,    websec_get);
    register_uri("/api/websec",          HTTP_PUT,    websec_put);

    // NTP + event log
    register_uri("/api/ntp",             HTTP_GET,    ntp_get);
    register_uri("/api/ntp",             HTTP_PUT,    ntp_put);
    register_uri("/api/eventlog",        HTTP_GET,    eventlog_get);

    // Dashboard
    register_uri("/api/dashboard/regs",  HTTP_GET,    dashboard_regs_get);
    register_uri("/api/dashboard/bits",  HTTP_GET,    dashboard_bits_get);

    // OTA
    register_uri("/api/ota/upload",      HTTP_POST,   ota_upload_post);
    register_uri("/api/ota/url",         HTTP_POST,   ota_url_post);
    register_uri("/api/ota/status",      HTTP_GET,    ota_status_get);

    // Sysparams log
    register_uri("/api/sysparams/log",   HTTP_GET,    sysparams_log_get);
    register_uri("/api/sysparams/log",   HTTP_POST,   sysparams_log_post);

    // Config import/export
    register_uri("/api/config/export",   HTTP_GET,    config_export_get);
    register_uri("/api/config/import",   HTTP_POST,   config_import_post);

    ESP_LOGI(TAG, "URI handlers registered");
}
