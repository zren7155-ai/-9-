#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pc_eeg_bridge_init(void);
esp_err_t pc_eeg_bridge_start(void);

#ifdef __cplusplus
}
#endif
