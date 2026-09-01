#pragma once
#include <stdint.h>
#include "app_config.h"   // data_gen_mode_t

// ============================================================================
//  Generatore dati del SIMULATORE SLAVE RTU (SPEC sez. 5).
//
//  Popola lo store (g_store) coi valori che lo slave RTU espone. Due modalita'
//  (come il Node.js ss_sim_slave.js, via flag --animate):
//    - DATAGEN_STATIC : reg[N]=N (valori riconoscibili per il round-trip Modbus:
//                       leggo 55000 -> ottengo 55000 -> canale ok). Applicato
//                       UNA volta all'avvio; nessun task periodico.
//    - DATAGEN_ANIM   : 8 forme d'onda sui registri (seno/coseno/gaussiana/
//                       dente di sega/triangolare/quadra/rumore/rampa a gradini)
//                       + bit coil/discrete random. Aggiornate da un task
//                       periodico ogni g_cfg.anim_ms (tick).
//
//  Le forme d'onda e le costanti sono replicate FEDELMENTE da ss_sim_slave.js
//  (startAnimation): output 0..4095, PERIOD_MS 8000, sfasamento per indice.
// ============================================================================

// Avvia il generatore secondo g_cfg.datagen. In STATIC riempie lo store e
// ritorna; in ANIM crea il task di animazione. Da chiamare a boot, dopo
// reg_store_init() e app_config_init().
void data_gen_start(void);

// Riempie lo store coi valori statici reg[N]=N (holding e input) e bit
// alternati (coil/discrete). Esposto a parte perche' e' anche il primo passo
// da collaudare (SPEC sez. 5: "statici prima").
void data_gen_fill_static(void);
