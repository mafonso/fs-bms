#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

// Application events on the default event loop, so features can react to each
// other without calling each other directly:
//
//     esp_event_handler_register(APP_EVENT, APP_EVENT_BMS_POLLED, on_bms, NULL);
//     app_event_post(APP_EVENT_BMS_POLLED, &ev, sizeof(ev));
//
// Event data is copied by the loop. Handlers run on the event loop task: keep
// them short and never block. Add new IDs at the end of the enum.
ESP_EVENT_DECLARE_BASE(APP_EVENT);

typedef enum {
    APP_EVENT_BMS_POLLED,   // a BMS poll finished; data is app_bms_event_t
} app_event_id_t;

typedef struct {
    bool ok;            // false: the poll failed; bms_get_sample() keeps the last good one
} app_bms_event_t;

// Post to the default loop without blocking; fails if the loop queue is full.
esp_err_t app_event_post(app_event_id_t id, const void *data, size_t size);
