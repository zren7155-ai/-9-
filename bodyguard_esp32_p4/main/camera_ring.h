#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_ring_init(void);
esp_err_t camera_ring_init_with_i2c(i2c_master_bus_handle_t i2c_bus);
esp_err_t camera_ring_start(void);
esp_err_t camera_ring_export_event_window(event_window_t *window);
esp_err_t camera_ring_copy_latest_jpeg(uint8_t *dst, size_t cap, size_t *out_size, uint64_t *timestamp_ms);
bool camera_ring_is_ready(void);

#ifdef __cplusplus
}
#endif
