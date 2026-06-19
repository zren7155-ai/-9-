#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_upload_init(void);
esp_err_t wifi_upload_start(void);
bool wifi_upload_is_connected(void);
uint32_t wifi_upload_get_ip4_addr(void);

#ifdef __cplusplus
}
#endif
