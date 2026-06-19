#pragma once

#include "esp_err.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ai_feedback_parser_parse(const char *json, learning_ai_feedback_t *out);

#ifdef __cplusplus
}
#endif
