#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mjpeg_stream_init(void);
esp_err_t mjpeg_stream_start(void);

#ifdef __cplusplus
}
#endif
