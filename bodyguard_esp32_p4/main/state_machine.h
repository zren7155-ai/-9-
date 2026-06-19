#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATE_IDLE,
    STATE_MONITORING,
    STATE_ALERT,
    STATE_UPLOADING,
    STATE_ERROR
} system_state_t;

typedef struct {
    bool sensor_ok;
    bool eeg_ok;
    bool camera_ok;
    bool ble_ok;
    bool wifi_ok;
    uint8_t health_score;
} system_health_t;

esp_err_t state_machine_init(void);
system_state_t state_machine_get_state(void);
void state_machine_update_health(const system_health_t *health);
void state_machine_set_state(system_state_t new_state);
const system_health_t *state_machine_get_health(void);
const char *state_machine_state_name(system_state_t state);

#ifdef __cplusplus
}
#endif
