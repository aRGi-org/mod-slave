#include "json_utils.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static char s_errbuf[96];

static int read_number(const cJSON *o, const char *key, double *val)
{
    const cJSON *item = cJSON_GetObjectItem(o, key);
    if (!item) return 0;
    if (!cJSON_IsNumber(item)) return -1;
    *val = item->valuedouble;
    return 1;
}

static bool in_int_range(double v, double lo, double hi)
{
    if (isnan(v) || isinf(v)) return false;
    if (v < lo || v > hi) return false;
    return true;
}

jget_result_t json_get_u8(const cJSON *o, const char *key, uint8_t min, uint8_t max, uint8_t def, uint8_t *out)
{
    double v; int r = read_number(o, key, &v);
    if (r == 0)  { *out = def; return JGET_OK; }
    if (r == -1) return JGET_WRONG_TYPE;
    if (!in_int_range(v, min, max)) return JGET_OUT_OF_RANGE;
    *out = (uint8_t)v; return JGET_OK;
}

jget_result_t json_get_u16(const cJSON *o, const char *key, uint16_t min, uint16_t max, uint16_t def, uint16_t *out)
{
    double v; int r = read_number(o, key, &v);
    if (r == 0)  { *out = def; return JGET_OK; }
    if (r == -1) return JGET_WRONG_TYPE;
    if (!in_int_range(v, min, max)) return JGET_OUT_OF_RANGE;
    *out = (uint16_t)v; return JGET_OK;
}

jget_result_t json_get_u32(const cJSON *o, const char *key, uint32_t min, uint32_t max, uint32_t def, uint32_t *out)
{
    double v; int r = read_number(o, key, &v);
    if (r == 0)  { *out = def; return JGET_OK; }
    if (r == -1) return JGET_WRONG_TYPE;
    if (!in_int_range(v, (double)min, (double)max)) return JGET_OUT_OF_RANGE;
    *out = (uint32_t)v; return JGET_OK;
}

jget_result_t json_get_i32(const cJSON *o, const char *key, int32_t min, int32_t max, int32_t def, int32_t *out)
{
    double v; int r = read_number(o, key, &v);
    if (r == 0)  { *out = def; return JGET_OK; }
    if (r == -1) return JGET_WRONG_TYPE;
    if (!in_int_range(v, (double)min, (double)max)) return JGET_OUT_OF_RANGE;
    *out = (int32_t)v; return JGET_OK;
}

jget_result_t json_get_bool(const cJSON *o, const char *key, bool def, bool *out)
{
    const cJSON *item = cJSON_GetObjectItem(o, key);
    if (!item) { *out = def; return JGET_OK; }
    if (!cJSON_IsBool(item)) return JGET_WRONG_TYPE;
    *out = cJSON_IsTrue(item); return JGET_OK;
}

jget_result_t json_get_str(const cJSON *o, const char *key, char *dest, size_t size, bool required)
{
    const cJSON *item = cJSON_GetObjectItem(o, key);
    if (!item || !cJSON_IsString(item) || item->valuestring == NULL) {
        if (required) return JGET_WRONG_TYPE;
        if (size > 0) dest[0] = '\0';
        return JGET_OK;
    }
    if (required && item->valuestring[0] == '\0') return JGET_WRONG_TYPE;
    strncpy(dest, item->valuestring, size - 1);
    dest[size - 1] = '\0';
    return JGET_OK;
}

const char *jget_strerror(jget_result_t r, const char *key)
{
    switch (r) {
        case JGET_OUT_OF_RANGE:
            snprintf(s_errbuf, sizeof(s_errbuf), "campo '%s' fuori range", key);
            break;
        case JGET_WRONG_TYPE:
            snprintf(s_errbuf, sizeof(s_errbuf), "campo '%s' mancante o di tipo errato", key);
            break;
        default:
            snprintf(s_errbuf, sizeof(s_errbuf), "campo '%s' ok", key);
            break;
    }
    return s_errbuf;
}