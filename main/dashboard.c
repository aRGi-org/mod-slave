// =============================================================================
//  dashboard.c - Serializzazione store per Dashboard (versione SIMULATORE)
// =============================================================================
//  Fork ridotto del dashboard del gateway. Espone lo store (holding/input/coils/
//  discrete) in JSON per la Dashboard UI. RIMOSSA la "mappa tipi" per-registro
//  (build_use_map), che nel gateway derivava dai blocchi di configurazione dei
//  device: il simulatore non ha device ne' blocchi, quindi tutti i registri
//  sono marcati UNUSED (la UI li mostra col colore neutro). Il resto (valori in
//  hex, bit) e' identico al gateway.
// =============================================================================
#include "dashboard.h"

#include <stdio.h>
#include <string.h>
#include "reg_store.h"
#include "app_config.h"

int dashboard_regs_json(int area, int start, int count, char *out, size_t out_len)
{
    if (area != 0 && area != 1) return -1;
    const uint16_t *src = (area == 0) ? g_store.holding : g_store.input;
    int area_qty = (area == 0) ? REG_HOLDING_QTY : REG_INPUT_QTY;

    if (start < 0) start = 0;
    if (start >= area_qty) return -1;
    if (count < 0) count = 0;
    if (start + count > area_qty) count = area_qty - start;

    // Copia sicura dei valori sotto lock (breve).
    static uint16_t vals[256];
    reg_lock();
    for (int i = 0; i < count; i++) vals[i] = src[start + i];
    reg_unlock();

    const char *area_name = (area == 0) ? "holding" : "input";
    int w = snprintf(out, out_len,
                     "{\"area\":\"%s\",\"start\":%d,\"count\":%d,\"vals\":[",
                     area_name, start, count);
    if (w < 0 || (size_t)w >= out_len) return -1;

    for (int i = 0; i < count; i++) {
        int k = snprintf(out + w, out_len - w, "%s\"0x%04X\"",
                         (i ? "," : ""), vals[i]);
        if (k < 0 || (size_t)(w + k) >= out_len) return -1;
        w += k;
    }

    int k = snprintf(out + w, out_len - w, "],\"use\":[");
    if (k < 0 || (size_t)(w + k) >= out_len) return -1;
    w += k;

    // Simulatore: nessun blocco -> tutti i registri UNUSED (REG_USE_UNUSED = 0).
    for (int i = 0; i < count; i++) {
        int kk = snprintf(out + w, out_len - w, "%s0", (i ? "," : ""));
        if (kk < 0 || (size_t)(w + kk) >= out_len) return -1;
        w += kk;
    }

    k = snprintf(out + w, out_len - w, "]}");
    if (k < 0 || (size_t)(w + k) >= out_len) return -1;
    w += k;
    return w;
}

int dashboard_bits_json(int area, char *out, size_t out_len)
{
    if (area != 2 && area != 3) return -1;
    const uint8_t *src = (area == 2) ? g_store.coils : g_store.discrete;
    int qty = (area == 2) ? REG_COILS_QTY : REG_DISCRETE_QTY;

    // Copia dei bit sotto lock.
    static uint8_t bits[256];
    reg_lock();
    for (int i = 0; i < qty; i++) {
        bits[i] = (src[i / 8] >> (i % 8)) & 0x01;
    }
    reg_unlock();

    const char *area_name = (area == 2) ? "coils" : "discrete";
    int w = snprintf(out, out_len, "{\"area\":\"%s\",\"count\":%d,\"bits\":[",
                     area_name, qty);
    if (w < 0 || (size_t)w >= out_len) return -1;

    for (int i = 0; i < qty; i++) {
        int k = snprintf(out + w, out_len - w, "%s%d", (i ? "," : ""), bits[i]);
        if (k < 0 || (size_t)(w + k) >= out_len) return -1;
        w += k;
    }
    int k = snprintf(out + w, out_len - w, "]}");
    if (k < 0 || (size_t)(w + k) >= out_len) return -1;
    w += k;
    return w;
}
