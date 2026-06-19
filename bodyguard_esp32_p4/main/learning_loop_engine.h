#pragma once

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t learning_loop_engine_init(void);
esp_err_t learning_loop_engine_start(void);
void learning_loop_engine_record_risk(const risk_sample_t *risk);
void learning_loop_engine_record_ai(const char *event_id, uint8_t ai_result);

#ifdef __cplusplus
}
#endif
