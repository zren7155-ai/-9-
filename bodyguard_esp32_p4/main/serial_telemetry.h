#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t serial_telemetry_init(void);
esp_err_t serial_telemetry_start(void);

#ifdef __cplusplus
}
#endif
