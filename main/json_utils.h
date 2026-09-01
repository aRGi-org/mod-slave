#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"

typedef enum {
    JGET_OK = 0,
    JGET_OUT_OF_RANGE,
    JGET_WRONG_TYPE
} jget_result_t;

jget_result_t json_get_u8 (const cJSON *o, const char *key, uint8_t  min, uint8_t  max, uint8_t  def, uint8_t  *out);
jget_result_t json_get_u16(const cJSON *o, const char *key, uint16_t min, uint16_t max, uint16_t def, uint16_t *out);
jget_result_t json_get_u32(const cJSON *o, const char *key, uint32_t min, uint32_t max, uint32_t def, uint32_t *out);
jget_result_t json_get_i32(const cJSON *o, const char *key, int32_t  min, int32_t  max, int32_t  def, int32_t  *out);
jget_result_t json_get_bool(const cJSON *o, const char *key, bool def, bool *out);
jget_result_t json_get_str(const cJSON *o, const char *key, char *dest, size_t size, bool required);
const char *jget_strerror(jget_result_t r, const char *key);