#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensor_init(QueueHandle_t output_queue);
esp_err_t sensor_start(void);
i2c_master_bus_handle_t sensor_get_i2c_bus(void);
sensor_sample_t sensor_get_latest(void);

#ifdef __cplusplus
}
#endif
