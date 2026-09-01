// =============================================================================
//  sys_wdt.c - Task Watchdog Timer applicativo
// =============================================================================
#include "sys_wdt.h"

#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "sys_wdt";
static bool s_inited = false;

void sys_wdt_init(void)
{
    if (s_inited) return;

    // Config del TWDT: timeout in ms, sorveglianza dei core idle disattivata
    // (sorvegliamo NOI i task applicativi, non gli idle - cosi' evitiamo falsi
    // positivi da task di sistema legittimamente occupati). trigger_panic=true:
    // allo scatto il sistema va in panic -> stampa backtrace del task bloccato e
    // resetta (recupero automatico + informazione per il debug).
    esp_task_wdt_config_t cfg = {
        .timeout_ms = SYS_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,      // non sorvegliare i task idle
        .trigger_panic = true,    // panic+reset+backtrace allo scatto
    };

    esp_err_t err;
#if defined(CONFIG_ESP_TASK_WDT_INIT)
    // L'IDF ha gia' inizializzato il TWDT all'avvio (CONFIG_ESP_TASK_WDT_INIT).
    // Chiamare esp_task_wdt_init() qui farebbe stampare alla libreria un errore
    // "TWDT already initialized". Evitiamo: riconfiguriamo direttamente col nostro
    // timeout e le nostre impostazioni.
    err = esp_task_wdt_reconfigure(&cfg);
#else
    // Il TWDT non e' auto-inizializzato: lo inizializziamo noi. Nel raro caso
    // fosse comunque gia' attivo, ripieghiamo sul reconfigure.
    err = esp_task_wdt_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&cfg);
    }
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TWDT init failed (%s)", esp_err_to_name(err));
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "task watchdog armed, timeout %d s", SYS_WDT_TIMEOUT_S);
}

void sys_wdt_task_register(void)
{
    if (!s_inited) return;
    esp_err_t err = esp_task_wdt_add(NULL);   // NULL = task chiamante
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG /* gia' registrato */) {
        ESP_LOGW(TAG, "wdt add failed (%s)", esp_err_to_name(err));
    }
}

void sys_wdt_task_kick(void)
{
    if (!s_inited) return;
    // Ignora l'errore: se il task non e' registrato, e' un no-op logico.
    esp_task_wdt_reset();
}

void sys_wdt_task_unregister(void)
{
    if (!s_inited) return;
    esp_task_wdt_delete(NULL);
}
