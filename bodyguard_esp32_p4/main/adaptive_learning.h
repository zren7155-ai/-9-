#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t adaptive_learning_init(void);
adaptive_params_t adaptive_learning_get_params(void);
uint8_t adaptive_learning_score(uint8_t pose_score, uint8_t eeg_score, uint8_t burst_score);
esp_err_t adaptive_learning_apply_feedback(const learning_ai_feedback_t *feedback);
esp_err_t adaptive_learning_apply_closed_loop(uint8_t risk_prediction,
                                              uint8_t ai_result,
                                              uint8_t pose_score,
                                              uint8_t eeg_score,
                                              uint8_t burst_score,
                                              const char *event_id);

#ifdef __cplusplus
}
#endif
