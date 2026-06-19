#pragma once

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t risk_engine_init(QueueHandle_t sensor_queue, QueueHandle_t eeg_queue);
esp_err_t risk_engine_start(void);
risk_sample_t risk_engine_get_latest(void);
void risk_engine_export_event_series(event_window_t *window);
void risk_engine_bind_event_id(const char *event_id);
void risk_engine_demo_set_latest(const risk_sample_t *risk);
void risk_engine_apply_ai_result(const char *event_id, uint8_t ai_result);
void risk_engine_apply_ai_report(const char *event_id, uint8_t ai_result, const char *report);

#ifdef __cplusplus
}
#endif
