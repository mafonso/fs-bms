// Felicity Solar BMS protocol: JSON frame reassembly and decoding. No BLE or
// IDF calls, so it builds and tests on the host too (test/test_bms_proto.c).
//
// Ported from aiobmsble aiobmsble/bms/felicity_bms.py (Apache-2.0,
// https://github.com/patman15/aiobmsble). Protocol reference: docs/felicity-protocol.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define BMS_CMD_REAL  "wifilocalMonitor:get dev real infor"
#define BMS_CMD_BASIC "wifilocalMonitor:get dev basice infor"  // "basice" is the protocol's spelling
#define BMS_CMD_DATE  "wifilocalMonitor:get Date"

#define BMS_SVC_UUID "6e6f736a-4643-4d44-8fa9-0fafd005e455"
#define BMS_TX_UUID  "49535258-184d-4bd9-bc61-20c647249616"  // write
#define BMS_RX_UUID  "49535458-8341-43f4-a9d4-ec0e34729bb3"  // notify

#define BMS_FRAME_MAX 4096
#define BMS_MAX_CELLS 32
#define BMS_MAX_TEMPS 16

// One reply is a JSON object split over several notifications: a chunk that
// starts with '{' starts a frame, chunks are appended until one ends with '}'
// and the whole thing parses.
typedef struct {
    char buf[BMS_FRAME_MAX + 1];
    size_t len;
} bms_frame_t;

typedef enum {
    BMS_FRAME_PENDING,    // need more chunks
    BMS_FRAME_DONE,       // *out is the parsed object with CommVer == 1 (caller frees)
    BMS_FRAME_BAD_VER,    // parsed, but CommVer != 1; dropped
    BMS_FRAME_OVERFLOW,   // longer than BMS_FRAME_MAX; dropped
} bms_frame_result_t;

void bms_frame_reset(bms_frame_t *f);
bms_frame_result_t bms_frame_feed(bms_frame_t *f, const uint8_t *data, size_t len, cJSON **out);

typedef struct {
    float voltage;          // V
    float current;          // A, positive while charging
    float soc;              // %
    float remaining_ah;     // Ah
    float capacity_ah;      // Ah, BatsocList[0][2] / 1000
    int n_cells;
    float cells[BMS_MAX_CELLS];   // V
    float cell_min, cell_max, cell_delta;
    int cell_min_idx, cell_max_idx;   // 0-based
    int n_temps;
    float temps[BMS_MAX_TEMPS];   // °C
    uint32_t warn, fault;
    int64_t problem;        // Bwarn + Bfault
} bms_sample_t;

// Decode a "get dev real infor" reply. On failure returns false and puts the
// reason in err.
bool bms_decode_real(const cJSON *msg, bms_sample_t *out, char *err, size_t err_len);

typedef struct {
    char serial[32];    // DevSN
    char sw[16];        // version
    char fw[16];        // M1SwVer
    char model[16];     // Type
} bms_info_t;

// Decode a "get dev basice infor" reply. Missing fields are left empty.
void bms_decode_basic(const cJSON *msg, bms_info_t *out);
