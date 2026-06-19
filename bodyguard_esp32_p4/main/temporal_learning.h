#pragma once

#include "esp_err.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t temporal_learning_init(void);
esp_err_t temporal_learning_start(void);
temporal_learning_snapshot_t temporal_learning_get_snapshot(void);
esp_err_t temporal_learning_on_ai_response(const char *event_id, const char *response);

#ifdef __cplusplus
}
#endif
