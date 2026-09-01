#pragma once

// ============================================================================
//  Configurazione hardware del SIMULATORE SLAVE RTU (fork di aRgiModGw).
//  A differenza del gateway (due porte COM1/COM2 a ruoli), il simulatore ha
//  UNA SOLA porta seriale, sempre in ruolo slave RTU.
//  Pinout ereditata dalla COM1 del gateway (gia' rodata): TX=26, RX=25, DE=27.
// ============================================================================

// ---- Unica UART per Modbus RTU ----
// UART0 e' la console/log: NON usarla. Usiamo UART1.
// La terna TX/RX/DE e' la STESSA per tutte e 4 le modalita' fisiche
// (UART TTL / RS-232 / RS-485 / RS-422): cambia solo il transceiver esterno
// e la selezione via web. In UART TTL e RS-232 il pin DE e' ignorato; in
// RS-485 pilota la direzione (automatico); in RS-422 resta fisso attivo.
#define SIM_UART_PORT    1
#define SIM_UART_TXD     26
#define SIM_UART_RXD     25
#define SIM_UART_RTS     27    // pin DE/RE del transceiver (RS-485/RS-422)

// Baud di default (allineato al gateway RTU: vedi SPEC sez. 6 -> 38400, 8N1).
#define SIM_UART_BAUD    38400

// Indirizzo slave di default (unit_id), poi configurabile via web.
#define SIM_SLAVE_ADDR   1

// ---- Pin di reset/config WiFi (FISSO, non configurabile) ----
// GPIO0 = BOOT button su gran parte delle dev board. Premuto = LOW.
//   - pressione BREVE (<1s)   -> riaccende l'AP per una finestra di config
//   - pressione LUNGA (>=10s) -> cancella credenziali WiFi + riavvia
#define WIFI_BTN_GPIO            0
#define WIFI_RESET_HOLD_MS       10000   // pressione lunga = reset totale
#define WIFI_SHORT_PRESS_MAX_MS  1000    // sotto questa soglia = pressione breve
#define WIFI_AP_LINGER_MS        180000  // 3 min: AP acceso dopo STA connessa / pressione breve

// ---- Configurazione rete AP (config/captive) ----
// UNICA fonte di verita' per l'indirizzo dell'AP. Scegli una subnet che non
// confligga con le reti dei clienti.
#define AP_IP_ADDR      "10.12.165.60"
#define AP_NETMASK      "255.255.255.0"

// ---- Baudrate RTU ammessi (lista chiusa; per aggiungerne, ricompilare) ----
#define RTU_ALLOWED_BAUDS  { 9600, 19200, 38400, 57600, 115200 }
