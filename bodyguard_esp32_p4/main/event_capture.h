#pragma once

#include "esp_err.h"
#include "event_router.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t event_capture_init(void);
esp_err_t event_capture_start(void);
bool event_capture_post(const event_trigger_t *trigger);

#ifdef __cplusplus
}
#endif
