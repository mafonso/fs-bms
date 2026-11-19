#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bms_proto.h"

#define MAX_VAL 0xFFFFFFFFLL        // JSON numbers are limited to 32 bits
#define NO_CELL 0xFFFF              // padding in unused BatcelList slots
#define NO_TEMP 0x7FFF              // temperature sensor not fitted

void bms_frame_reset(bms_frame_t *f)
{
    f->len = 0;
    f->buf[0] = '\0';
}

bms_frame_result_t bms_frame_feed(bms_frame_t *f, const uint8_t *data, size_t len, cJSON **out)
{
    *out = NULL;
    // Some stacks pad notifications; trailing NULs or whitespace must not hide a closing '}'.
    while (len > 0 && (data[len - 1] == '\0' || data[len - 1] == '\r' || data[len - 1] == '\n' ||
                       data[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) {
        return BMS_FRAME_PENDING;
    }
    if (data[0] == '{') {
        bms_frame_reset(f);
    } else if (f->len == 0) {
        return BMS_FRAME_PENDING;   // tail of a frame we missed the start of
    }
    if (f->len + len > BMS_FRAME_MAX) {
        bms_frame_reset(f);
        return BMS_FRAME_OVERFLOW;
    }
    memcpy(f->buf + f->len, data, len);
    f->len += len;
    f->buf[f->len] = '\0';

    if (data[len - 1] != '}') {
        return BMS_FRAME_PENDING;
    }
    // A '}' that closes a nested object can end a chunk too: then it doesn't
    // parse yet and we keep appending.
    cJSON *msg = cJSON_ParseWithLength(f->buf, f->len);
    if (!msg) {
        return BMS_FRAME_PENDING;
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(msg, "CommVer");
    if (!cJSON_IsNumber(ver) || ver->valuedouble != 1) {
        cJSON_Delete(msg);
        return BMS_FRAME_BAD_VER;
    }
    *out = msg;
    return BMS_FRAME_DONE;
}

// A JSON integer, possibly sent as a string, within +-0xFFFFFFFF.
static bool json_int(const cJSON *v, int64_t *out)
{
    double d;
    if (cJSON_IsNumber(v)) {
        d = v->valuedouble;
    } else if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        d = strtod(v->valuestring, &end);
        if (*end) {
            return false;
        }
    } else {
        return false;
    }
    if (d != floor(d) || fabs(d) > (double)MAX_VAL) {
        return false;
    }
    *out = (int64_t)d;
    return true;
}

// msg[key][row][col] as an integer.
static bool field(const cJSON *msg, const char *key, int row, int col, int64_t *out)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(msg, key);
    return json_int(cJSON_GetArrayItem(cJSON_GetArrayItem(a, row), col), out);
}

static bool fail(char *err, size_t err_len, const char *what)
{
    snprintf(err, err_len, "%s", what);
    return false;
}

bool bms_decode_real(const cJSON *msg, bms_sample_t *s, char *err, size_t err_len)
{
    memset(s, 0, sizeof(*s));
    int64_t v, soc, cap;

    if (!field(msg, "BattList", 0, 0, &v)) {
        return fail(err, err_len, "BattList[0][0] (voltage) missing");
    }
    s->voltage = v / 1000.0f;
    if (!field(msg, "BattList", 1, 0, &v)) {
        return fail(err, err_len, "BattList[1][0] (current) missing");
    }
    s->current = v / 10.0f;
    if (!field(msg, "BatsocList", 0, 0, &soc) || !field(msg, "BatsocList", 0, 2, &cap)) {
        return fail(err, err_len, "BatsocList[0] (SoC, capacity) missing");
    }
    s->soc = soc / 100.0f;
    s->remaining_ah = (float)((double)soc * (double)cap / 1e7);
    s->capacity_ah = cap / 1000.0f;

    const cJSON *cells = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(msg, "BatcelList"), 0);
    if (!cJSON_IsArray(cells)) {
        return fail(err, err_len, "BatcelList[0] (cells) missing");
    }
    const cJSON *c;
    cJSON_ArrayForEach(c, cells) {
        if (!json_int(c, &v)) {
            return fail(err, err_len, "bad cell value");
        }
        if (v == NO_CELL) {
            continue;
        }
        if (s->n_cells == BMS_MAX_CELLS) {
            return fail(err, err_len, "too many cells");
        }
        s->cells[s->n_cells++] = v / 1000.0f;
    }
    if (s->n_cells == 0) {
        return fail(err, err_len, "no cell voltages");
    }
    s->cell_min = s->cell_max = s->cells[0];
    for (int i = 1; i < s->n_cells; i++) {
        if (s->cells[i] < s->cell_min) {
            s->cell_min = s->cells[i];
            s->cell_min_idx = i;
        }
        if (s->cells[i] > s->cell_max) {
            s->cell_max = s->cells[i];
            s->cell_max_idx = i;
        }
    }
    // From the integer millivolts, so 3.389 - 3.378 is exactly 0.011.
    s->cell_delta = roundf((s->cell_max - s->cell_min) * 1000.0f) / 1000.0f;

    const cJSON *temps = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(msg, "BtemList"), 0);
    if (!cJSON_IsArray(temps)) {
        return fail(err, err_len, "BtemList[0] (temperatures) missing");
    }
    cJSON_ArrayForEach(c, temps) {
        if (!json_int(c, &v)) {
            return fail(err, err_len, "bad temperature value");
        }
        // 0x7FFF: no sensor. 0xFFFF (padding) would read as 6553.5 °C; skip it too.
        if (v == NO_TEMP || v == 0xFFFF || s->n_temps == BMS_MAX_TEMPS) {
            continue;
        }
        s->temps[s->n_temps++] = v / 10.0f;
    }

    int64_t warn = 0, fault = 0;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(msg, "Bwarn");
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(msg, "Bfault");
    if ((w && !json_int(w, &warn)) || (f && !json_int(f, &fault))) {
        return fail(err, err_len, "bad Bwarn/Bfault");
    }
    s->warn = (uint32_t)warn;
    s->fault = (uint32_t)fault;
    s->problem = warn + fault;
    return true;
}

static void copy_field(const cJSON *msg, const char *key, char *dst, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, key);
    if (cJSON_IsString(v)) {
        snprintf(dst, len, "%s", v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        snprintf(dst, len, "%.0f", v->valuedouble);
    } else {
        dst[0] = '\0';
    }
}

void bms_decode_basic(const cJSON *msg, bms_info_t *out)
{
    copy_field(msg, "DevSN", out->serial, sizeof(out->serial));
    copy_field(msg, "version", out->sw, sizeof(out->sw));
    copy_field(msg, "M1SwVer", out->fw, sizeof(out->fw));
    copy_field(msg, "Type", out->model, sizeof(out->model));
}
