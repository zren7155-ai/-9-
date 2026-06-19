#pragma once

#include "esp_err.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t learning_storage_init(void);
esp_err_t learning_storage_load_params(adaptive_params_t *params);
esp_err_t learning_storage_save_params(const adaptive_params_t *params);
esp_err_t learning_storage_cache_ai_feedback(const learning_ai_feedback_t *feedback);

#ifdef __cplusplus
}
#endif
