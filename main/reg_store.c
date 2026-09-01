#include "reg_store.h"
#include <string.h>

reg_store_t g_store;
portMUX_TYPE g_store_spinlock = portMUX_INITIALIZER_UNLOCKED;

void reg_store_init(void)
{
    memset(&g_store, 0, sizeof(g_store));
}

// --- conversione pura word<->float per i 4 layout ---

float float_from_words(uint16_t reg0, uint16_t reg1, float_layout_t layout)
{
    // Convenzione: il float e' A(msb) B C D(lsb).
    uint8_t b[4];   // b[0]=A .. b[3]=D, ordine "naturale" big-endian

    uint8_t r0_hi = reg0 >> 8, r0_lo = reg0 & 0xFF;
    uint8_t r1_hi = reg1 >> 8, r1_lo = reg1 & 0xFF;

    switch (layout) {
        case FLOAT_ABCD:  b[0]=r0_hi; b[1]=r0_lo; b[2]=r1_hi; b[3]=r1_lo; break;
        case FLOAT_CDAB:  b[0]=r1_hi; b[1]=r1_lo; b[2]=r0_hi; b[3]=r0_lo; break;
        case FLOAT_BADC:  b[0]=r0_lo; b[1]=r0_hi; b[2]=r1_lo; b[3]=r1_hi; break;
        case FLOAT_DCBA:  b[0]=r1_lo; b[1]=r1_hi; b[2]=r0_lo; b[3]=r0_hi; break;
        default:          b[0]=r0_hi; b[1]=r0_lo; b[2]=r1_hi; b[3]=r1_lo; break;
    }

    uint32_t raw = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
    float f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

void float_to_words(float value, uint16_t *reg0, uint16_t *reg1, float_layout_t layout)
{
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));

    uint8_t b[4];   // b[0]=A(msb) .. b[3]=D(lsb)
    b[0] = raw >> 24; b[1] = raw >> 16; b[2] = raw >> 8; b[3] = raw;

    uint8_t r0_hi, r0_lo, r1_hi, r1_lo;
    switch (layout) {
        case FLOAT_ABCD:  r0_hi=b[0]; r0_lo=b[1]; r1_hi=b[2]; r1_lo=b[3]; break;
        case FLOAT_CDAB:  r0_hi=b[2]; r0_lo=b[3]; r1_hi=b[0]; r1_lo=b[1]; break;
        case FLOAT_BADC:  r0_hi=b[1]; r0_lo=b[0]; r1_hi=b[3]; r1_lo=b[2]; break;
        case FLOAT_DCBA:  r0_hi=b[3]; r0_lo=b[2]; r1_hi=b[1]; r1_lo=b[0]; break;
        default:          r0_hi=b[0]; r0_lo=b[1]; r1_hi=b[2]; r1_lo=b[3]; break;
    }
    *reg0 = ((uint16_t)r0_hi << 8) | r0_lo;
    *reg1 = ((uint16_t)r1_hi << 8) | r1_lo;
}

// --- versioni che leggono/scrivono direttamente lo store ---

float reg_get_float(int reg_index, float_layout_t layout)
{
    if (reg_index < 0 || reg_index + 1 >= REG_HOLDING_QTY) return 0.0f;
    return float_from_words(g_store.holding[reg_index],
                            g_store.holding[reg_index + 1], layout);
}

void reg_set_float(int reg_index, float value, float_layout_t layout)
{
    if (reg_index < 0 || reg_index + 1 >= REG_HOLDING_QTY) return;
    float_to_words(value, &g_store.holding[reg_index],
                          &g_store.holding[reg_index + 1], layout);
}
