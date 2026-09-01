#pragma once
// =============================================================================
//  sys_wdt - Task Watchdog Timer (TWDT) applicativo
// =============================================================================
//  Sorveglia i task critici: se uno si blocca e non "da' il colpo" (kick) entro
//  il timeout, il watchdog stampa un backtrace del task bloccato e resetta il
//  gateway (recupero automatico). Complementa l'auto-recupero del master TCP:
//  quello copre un caso noto (INVALID_STATE), il watchdog copre l'ignoto
//  (deadlock, loop infinito, task appeso).
//
//  Uso:
//   1. sys_wdt_init() una volta all'avvio (configura il TWDT, timeout 20s).
//   2. Ogni task critico chiama sys_wdt_task_register() all'inizio e
//      sys_wdt_task_kick() a ogni giro del suo loop.
//
//  Task sorvegliati: master TCP, slave TCP, master RTU, slave RTU. Il web server
//  (task interno a esp_http_server) e' sorvegliato indirettamente da un task
//  sentinella opzionale (vedi sys_wdt_web_alive).
// =============================================================================

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Timeout del watchdog in secondi. 20s: ampio margine anche per attese Modbus
// (una lettura RTU/TCP con timeout sta ben sotto). >=10s come richiesto.
#define SYS_WDT_TIMEOUT_S   20

// Inizializza il Task Watchdog Timer. Da chiamare una volta all'avvio, prima di
// creare i task critici. Idempotente.
void sys_wdt_init(void);

// Registra il task CHIAMANTE al watchdog. Da chiamare dall'interno del task
// (all'inizio, prima del loop). Dopo la registrazione il task DEVE chiamare
// sys_wdt_task_kick() entro il timeout, altrimenti scatta il reset.
void sys_wdt_task_register(void);

// "Da' il colpo" al watchdog per il task chiamante: segnala che e' vivo. Da
// chiamare a ogni giro del loop del task. Sicura anche se il task non e'
// registrato (no-op).
void sys_wdt_task_kick(void);

// Deregistra il task chiamante (es. se sta per terminare). Raramente necessaria.
void sys_wdt_task_unregister(void);

#ifdef __cplusplus
}
#endif
