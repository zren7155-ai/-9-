#pragma once

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_init(void);
esp_err_t ble_start(void);
bool ble_is_ready(void);

#ifdef __cplusplus
}
#endif
