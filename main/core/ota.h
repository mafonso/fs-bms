#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_DOWNLOADING,
    OTA_REBOOTING,
    OTA_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    bool checked;           // a check has completed successfully
    bool update_available;  // latest is newer than the running version
    char latest_version[32];
    char latest_url[256];
    char latest_notes[256];
    int bytes_read;
    int total_bytes;
    char error[96];
} ota_status_t;

void ota_init(void);
const char *ota_state_name(ota_state_t state);

// If this image is pending verification, confirm it once Wi-Fi is up, else roll back.
void ota_confirm_or_rollback(void);

// Fetch the release manifest and compare it with the running version (blocking).
esp_err_t ota_check(void);

// The same on a task of its own (the TLS handshake needs a large stack); poll
// ota_get_status() until the state leaves OTA_CHECKING.
esp_err_t ota_check_async(void);

// Download and install the version found by the last check, then reboot (async).
esp_err_t ota_start_update(void);

void ota_get_status(ota_status_t *out);

// True while a check or an update runs: radio- and memory-heavy work (BLE) should
// not start, since Wi-Fi shares the radio and a TLS session needs ~40 KB of heap.
bool ota_busy(void);

// Called on the OTA path before each TLS connection, once ota_busy() is true. A hook
// returns when the caller's own heavy work has finished or been closed (bounded wait).
// Register in a feature's init; up to 4.
esp_err_t ota_add_quiesce_hook(void (*fn)(void));
