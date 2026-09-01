#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"

// ============================================================================
//  Store dei registri Modbus condiviso tra tutti i controller (slave/master,
//  RTU/TCP). Unica fonte di verita' in RAM.
//  Sincronizzazione: spinlock (portMUX_TYPE), come prescritto da Espressif
//  per aree condivise tra piu' oggetti slave. Le sezioni critiche DEVONO
//  restare cortissime: dentro portENTER_CRITICAL gli interrupt sono spenti.
// ============================================================================
/*
#define REG_COILS_QTY       128   // bit
#define REG_DISCRETE_QTY    128   // bit
#define REG_HOLDING_QTY     128   // registri 16-bit
#define REG_INPUT_QTY       128   // registri 16-bit
*/

#define REG_COILS_QTY       256   // bit
#define REG_DISCRETE_QTY    256   // bit
#define REG_HOLDING_QTY     256   // registri 16-bit
#define REG_INPUT_QTY       256   // registri 16-bit

// I quattro layout di un float/uint32 su due registri.
// Lettera = ordine dei byte sul filo, dal primo registro al secondo.
// A = byte piu' significativo del float ... D = meno significativo.
typedef enum {
    FLOAT_ABCD,   // big-endian pieno:  reg[0]=AB, reg[1]=CD
    FLOAT_CDAB,   // word-swap:         reg[0]=CD, reg[1]=AB
    FLOAT_BADC,   // byte-swap in word: reg[0]=BA, reg[1]=DC
    FLOAT_DCBA    // little-endian pieno
} float_layout_t;

// Alias retrocompatibili (word order a 2 valori).
#define WORD_ORDER_AB  FLOAT_ABCD
#define WORD_ORDER_BA  FLOAT_CDAB
typedef float_layout_t word_order_t;

typedef struct {
    uint16_t holding[REG_HOLDING_QTY];        // FC 03/06/16
    uint16_t input[REG_INPUT_QTY];            // FC 04
    uint8_t  coils[REG_COILS_QTY / 8];        // FC 01/05/15
    uint8_t  discrete[REG_DISCRETE_QTY / 8];  // FC 02
} reg_store_t;

extern reg_store_t g_store;
extern portMUX_TYPE g_store_spinlock;

void reg_store_init(void);

#define reg_lock()   portENTER_CRITICAL(&g_store_spinlock)
#define reg_unlock() portEXIT_CRITICAL(&g_store_spinlock)

// Conversione float <-> due holding register consecutivi (accesso allo store).
// reg_index = indice del PRIMO dei due registri. NON prendono il lock.
float reg_get_float(int reg_index, float_layout_t layout);
void  reg_set_float(int reg_index, float value, float_layout_t layout);

// Conversione pura su una coppia di word gia' in mano (non tocca lo store).
// Utile ai master: convertono cio' che hanno letto dal device.
float float_from_words(uint16_t reg0, uint16_t reg1, float_layout_t layout);
void  float_to_words(float value, uint16_t *reg0, uint16_t *reg1, float_layout_t layout);
