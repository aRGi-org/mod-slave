#include "mb_slave_rtu.h"
#include "reg_store.h"
#include "board_config.h"
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include "mbcontroller.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
//  Slave Modbus RTU del SIMULATORE. Fork ridotto del mb_slave_rtu del gateway:
//   - UNA sola porta (UART1), non piu' parametrico su port_idx / g_cfg.ports[].
//   - NIENTE aree diagnostiche mb_diag (@55000/@60000): il simulatore deve
//     poter esporre QUALSIASI indirizzo col generatore dati (es. statici
//     reg[N]=N a 55000 -> 55000), quindi quelle aree riservate sparirebbero
//     in conflitto. Espone solo ed esattamente lo store.
//   - AGGIUNTA la modalita' fisica della porta (UART/RS232/RS485/RS422),
//     letta da g_cfg.phys_mode (SPEC sez. 4).
//  Espone SEMPRE tutto lo store (le 4 aree). Parametri seriali da g_cfg.serial.
// ============================================================================

static const char *TAG = "slave_rtu";

static void *s_handle = NULL;

static void set_area_ex(void *handle, mb_param_type_t type, uint16_t start_offset,
                        void *addr, size_t size_bytes, mb_param_access_t access)
{
    mb_register_area_descriptor_t area = {
        .type = type,
        .start_offset = start_offset,
        .address = addr,
        .size = size_bytes,
        .access = access,
    };
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(handle, area));
}

static void set_area(void *handle, mb_param_type_t type, void *addr, size_t size_bytes)
{
    set_area_ex(handle, type, 0, addr, size_bytes, MB_ACCESS_RW);
}

static void slave_task(void *arg)
{
    (void)arg;
    void *handle = s_handle;

    const int mask = MB_EVENT_HOLDING_REG_RD | MB_EVENT_HOLDING_REG_WR |
                     MB_EVENT_INPUT_REG_RD |
                     MB_EVENT_COILS_RD  | MB_EVENT_COILS_WR |
                     MB_EVENT_DISCRETE_RD;

    mb_param_info_t info;
    while (1) {
        (void)mbc_slave_check_event(handle, mask);
        if (mbc_slave_get_param_info(handle, &info, pdMS_TO_TICKS(10)) == ESP_OK) {
            const char *rw = (info.type & (MB_EVENT_HOLDING_REG_WR | MB_EVENT_COILS_WR))
                             ? "WR" : "RD";
            ESP_LOGI(TAG, "%s off=%u size=%u type=0x%x",
                     rw, (unsigned)info.mb_offset, (unsigned)info.size,
                     (unsigned)info.type);
        }
    }
}

static uart_parity_t parity_from_cfg(uint8_t p)
{
    switch (p) {
        case 1:  return UART_PARITY_ODD;
        case 2:  return UART_PARITY_EVEN;
        default: return UART_PARITY_DISABLE;
    }
}

static uart_word_length_t databits_from_cfg(uint8_t d)
{
    return (d == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
}

static uart_stop_bits_t stopbits_from_cfg(uint8_t s)
{
    return (s == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

// Applica la modalita' fisica della porta (SPEC sez. 4). Stessi pin TX/RX/DE
// per tutte; cambia il transceiver esterno e come configuriamo la UART.
//   UART_TTL : TTL diretto 3.3V, nessun transceiver, DE ignorato.
//   RS232    : MAX232 (+-12V). La UART lavora come TTL (il MAX232 traduce i
//              livelli fuori dall'ESP32), DE ignorato.
//   RS485    : half-duplex 2 fili, DE = direzione, pilotato AUTOMATICAMENTE
//              dal driver UART via il pin RTS (UART_MODE_RS485_HALF_DUPLEX).
//   RS422    : full-duplex 4 fili, due coppie differenziali sempre attive.
//              DE del driver di linea tenuto FISSO attivo: lo pilotiamo a mano
//              come GPIO alto, e la UART resta in modo normale.
static void apply_phys_mode(uint8_t uart_port, port_phys_mode_t mode)
{
    switch (mode) {
        case PHYS_RS485:
            // DE automatico via RTS: il pin RTS DEVE restare assegnato alla UART.
            ESP_ERROR_CHECK(uart_set_pin(uart_port, SIM_UART_TXD, SIM_UART_RXD,
                                         SIM_UART_RTS, UART_PIN_NO_CHANGE));
            ESP_ERROR_CHECK(uart_set_mode(uart_port, UART_MODE_RS485_HALF_DUPLEX));
            ESP_LOGI(TAG, "phys mode: RS-485 half-duplex (DE auto via RTS gpio%d)", SIM_UART_RTS);
            break;

        case PHYS_RS422:
            // Full-duplex: UART normale su TX/RX (RTS non gestito dalla UART).
            // DE del transceiver tenuto fisso attivo come GPIO.
            ESP_ERROR_CHECK(uart_set_pin(uart_port, SIM_UART_TXD, SIM_UART_RXD,
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
            ESP_ERROR_CHECK(uart_set_mode(uart_port, UART_MODE_UART));
            gpio_reset_pin(SIM_UART_RTS);
            gpio_set_direction(SIM_UART_RTS, GPIO_MODE_OUTPUT);
            gpio_set_level(SIM_UART_RTS, 1);   // DE fisso attivo (full-duplex)
            ESP_LOGI(TAG, "phys mode: RS-422 full-duplex (DE fixed high gpio%d)", SIM_UART_RTS);
            break;

        case PHYS_RS232:
            // MAX232 esterno: la UART lavora come TTL, DE non usato.
            ESP_ERROR_CHECK(uart_set_pin(uart_port, SIM_UART_TXD, SIM_UART_RXD,
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
            ESP_ERROR_CHECK(uart_set_mode(uart_port, UART_MODE_UART));
            ESP_LOGI(TAG, "phys mode: RS-232 (via MAX232, DE ignored)");
            break;

        case PHYS_UART_TTL:
        default:
            // TTL diretto 3.3V, nessun transceiver.
            ESP_ERROR_CHECK(uart_set_pin(uart_port, SIM_UART_TXD, SIM_UART_RXD,
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
            ESP_ERROR_CHECK(uart_set_mode(uart_port, UART_MODE_UART));
            ESP_LOGI(TAG, "phys mode: UART TTL 3.3V direct (DE ignored)");
            break;
    }
}

void mb_slave_rtu_start(void)
{
    const cfg_serial_t *p = &g_cfg.serial;
    uint8_t  uart_port = SIM_UART_PORT;
    uint32_t baud      = p->baudrate ? p->baudrate : SIM_UART_BAUD;
    uint8_t  modbus_id = p->modbus_id ? p->modbus_id : SIM_SLAVE_ADDR;

    mb_communication_info_t cfg = {
        .ser_opts.port      = uart_port,
        .ser_opts.mode      = MB_RTU,
        .ser_opts.baudrate  = baud,
        .ser_opts.parity    = parity_from_cfg(p->parity),
        .ser_opts.uid       = modbus_id,
        .ser_opts.data_bits = databits_from_cfg(p->data_bits),
        .ser_opts.stop_bits = stopbits_from_cfg(p->stop_bits),
    };
    ESP_ERROR_CHECK(mbc_slave_create_serial(&cfg, &s_handle));

    // Espone TUTTO lo store: le 4 aree, size in BYTE. NIENTE aree mb_diag.
    set_area(s_handle, MB_PARAM_HOLDING,  g_store.holding,  sizeof(g_store.holding));
    set_area(s_handle, MB_PARAM_INPUT,    g_store.input,    sizeof(g_store.input));
    set_area(s_handle, MB_PARAM_COIL,     g_store.coils,    sizeof(g_store.coils));
    set_area(s_handle, MB_PARAM_DISCRETE, g_store.discrete, sizeof(g_store.discrete));

    // Modalita' fisica della porta (dopo create_serial: esp-modbus ha gia'
    // installato il driver UART, qui ne cambiamo pin/mode secondo la config).
    apply_phys_mode(uart_port, g_cfg.phys_mode);

    ESP_ERROR_CHECK(mbc_slave_start(s_handle));

    xTaskCreate(slave_task, "mb_slv_rtu", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "slave RTU started on UART%d addr=%d baud=%lu (%s)",
             uart_port, modbus_id, (unsigned long)baud,
             phys_mode_str(g_cfg.phys_mode));
}
