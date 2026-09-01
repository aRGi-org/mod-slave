// =============================================================================
//  net_mdns.c - Annuncio mDNS
// =============================================================================
#include "net_mdns.h"
#include "web_server.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns";
static bool s_started = false;

void net_mdns_start(void)
{
    if (s_started) return;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }

    // Hostname -> il dispositivo diventa MDNS_HOSTNAME ".local".
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("aRgiMBSS ModBus Slave Simulator");

    // Pubblicizza il servizio HTTPS sulla porta 443 (compare nei tool di
    // discovery di rete come servizio web sicuro del dispositivo).
    // Annuncia il servizio web sul protocollo effettivamente attivo (HTTP o
    // HTTPS con fallback). web_server_is_https() riflette lo stato reale.
    if (web_server_is_https()) {
        mdns_service_add(NULL, "_https", "_tcp", 443, NULL, 0);
    } else {
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    s_started = true;
    ESP_LOGI(TAG, "mDNS started: %s.local (%s)", MDNS_HOSTNAME,
             web_server_is_https() ? "HTTPS on 443" : "HTTP on 80");
}
