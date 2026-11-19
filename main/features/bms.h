#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bms_proto.h"

// Copy of the last good sample and its age in seconds; false if there is none yet.
// Safe from any task. APP_EVENT_BMS_POLLED (app_events.h) fires after each poll.
bool bms_get_sample(bms_sample_t *out, uint32_t *age_s);
