#include "data_gen.h"
#include "reg_store.h"

#include <stdlib.h>
#include <math.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
//  Generatore dati. Vedi data_gen.h per la panoramica.
//  Le curve sono la trascrizione 1:1 di startAnimation() in ss_sim_slave.js
//  (letto per intero, come prescrive la SPEC sez. 5: NON dedurre, replicare).
// ============================================================================

static const char *TAG = "data_gen";

// ---- Helper bit-packing per coil/discrete (LSB-first, come esp-modbus) ----
static inline void bit_set(uint8_t *packed, int idx, int val)
{
    if (val) packed[idx >> 3] |=  (1u << (idx & 7));
    else     packed[idx >> 3] &= ~(1u << (idx & 7));
}
static inline int bit_get(const uint8_t *packed, int idx)
{
    return (packed[idx >> 3] >> (idx & 7)) & 1;
}

// ============================================================================
//  MODALITA' 1 - STATICI RICONOSCIBILI: reg[N] = N
// ============================================================================
// Nel Node.js lo store e' sparso (solo i registri dei blocchi) e usa
// recognizable(): 16-bit -> dev_start, 32-bit -> 10000+dev_start. Qui lo store
// e' DENSO (tutti i 256+256 registri): l'equivalente diretto e' reg[N]=N, che
// e' anche il caso d'uso descritto dalla SPEC ("leggo 55000 -> ottengo 55000").
// Per i bit (coil/discrete) alterniamo 0/1 come fa buildStore (base%2).
void data_gen_fill_static(void)
{
    reg_lock();
    for (int i = 0; i < REG_HOLDING_QTY; i++) g_store.holding[i] = (uint16_t)i;
    for (int i = 0; i < REG_INPUT_QTY;   i++) g_store.input[i]   = (uint16_t)i;
    for (int i = 0; i < REG_COILS_QTY;    i++) bit_set(g_store.coils,    i, (i % 2) == 0);
    for (int i = 0; i < REG_DISCRETE_QTY; i++) bit_set(g_store.discrete, i, (i % 2) == 0);
    reg_unlock();
    ESP_LOGI(TAG, "static fill: reg[N]=N (holding+input), bit[N]=N%%2 (coil+discrete)");
}

// ============================================================================
//  MODALITA' 2 - 8 FORME D'ONDA (replicate da ss_sim_slave.js)
// ============================================================================
// Ogni funzione riceve ph in [0,1) e ritorna un intero 0..4095 (12 bit, ben
// visibile in hex 0x000..0xFFF senza saturare). Costanti IDENTICHE al Node.js.
#define WAVE_OFF   2048.0   // centro per le onde bipolari
#define WAVE_AMP   1900.0   // ampiezza per le onde bipolari
#define WAVE_PERIOD_MS  8000.0   // periodo base di un ciclo d'onda

typedef int (*wave_fn)(double ph);

// 0: seno
static int w_sin(double ph)   { return (int)lround(WAVE_OFF + WAVE_AMP * sin(2.0 * M_PI * ph)); }
// 1: coseno
static int w_cos(double ph)   { return (int)lround(WAVE_OFF + WAVE_AMP * cos(2.0 * M_PI * ph)); }
// 2: gaussiana pulsante (picco che passa)
static int w_gauss(double ph) { double x = (ph - 0.5) * 6.0; return (int)lround(100.0 + 3900.0 * exp(-x * x)); }
// 3: dente di sega
static int w_saw(double ph)   { return (int)lround(100.0 + 3900.0 * ph); }
// 4: triangolare
static int w_tri(double ph)   { return (int)lround(100.0 + 3900.0 * (1.0 - fabs(2.0 * ph - 1.0))); }
// 5: onda quadra
static int w_square(double ph){ return (ph < 0.5) ? 3900 : 200; }
// 6: rumore (random ad ogni tick) -> floor(rand*4096)
static int w_noise(double ph) { (void)ph; return (int)(esp_random() % 4096u); }
// 7: rampa lenta a gradini (8 gradini)
static int w_steps(double ph) { return (int)lround(floor(ph * 8.0) / 7.0 * 3900.0 + 100.0); }

static const wave_fn WAVES[] = {
    w_sin, w_cos, w_gauss, w_saw, w_tri, w_square, w_noise, w_steps,
};
#define NWAVES (sizeof(WAVES) / sizeof(WAVES[0]))

static int64_t s_t0_us = 0;

// Aggiorna una volta tutti i registri (holding+input) con le forme d'onda e i
// bit (coil+discrete) in modo random. Replica animRegs/animBits del Node.js.
static void anim_tick(void)
{
    int64_t now_us = esp_timer_get_time();
    double basePh = fmod((double)(now_us - s_t0_us) / 1000.0, WAVE_PERIOD_MS) / WAVE_PERIOD_MS;

    reg_lock();

    // Registri: forma d'onda scelta per indice, con sfasamento per indice cosi'
    // onde uguali non sono sincronizzate (ph = basePh + i*0.13, mod 1).
    for (int i = 0; i < REG_HOLDING_QTY; i++) {
        double ph = fmod(basePh + i * 0.13, 1.0);
        g_store.holding[i] = (uint16_t)(WAVES[i % NWAVES](ph) & 0xFFFF);
    }
    for (int i = 0; i < REG_INPUT_QTY; i++) {
        double ph = fmod(basePh + i * 0.13, 1.0);
        g_store.input[i] = (uint16_t)(WAVES[i % NWAVES](ph) & 0xFFFF);
    }

    // Bit: 1/3 puramente casuali ad ogni tick (molto random), gli altri flip
    // occasionale ~15% per tick. (animBits del Node.js.)
    for (int i = 0; i < REG_COILS_QTY; i++) {
        if (i % 3 == 0)                 bit_set(g_store.coils, i, (esp_random() & 1));
        else if ((esp_random() % 100) < 15) bit_set(g_store.coils, i, !bit_get(g_store.coils, i));
    }
    for (int i = 0; i < REG_DISCRETE_QTY; i++) {
        if (i % 3 == 0)                 bit_set(g_store.discrete, i, (esp_random() & 1));
        else if ((esp_random() % 100) < 15) bit_set(g_store.discrete, i, !bit_get(g_store.discrete, i));
    }

    reg_unlock();
}

static void anim_task(void *arg)
{
    uint32_t period = (uint32_t)(uintptr_t)arg;
    if (period < 20) period = 20;   // guardia: tick minimo ragionevole
    s_t0_us = esp_timer_get_time();
    ESP_LOGI(TAG, "animation task: 8 waveforms + random bits, tick=%lums", (unsigned long)period);
    while (1) {
        anim_tick();
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

// ============================================================================
//  AVVIO
// ============================================================================
void data_gen_start(void)
{
    if (g_cfg.datagen == DATAGEN_ANIM) {
        uint32_t period = g_cfg.anim_ms ? g_cfg.anim_ms : 500;
        // Seed iniziale statico cosi' i registri hanno subito valori sensati
        // anche prima del primo tick.
        data_gen_fill_static();
        xTaskCreate(anim_task, "data_gen", 4096, (void *)(uintptr_t)period, 5, NULL);
    } else {
        data_gen_fill_static();   // STATIC: riempi una volta, nessun task
    }
}
