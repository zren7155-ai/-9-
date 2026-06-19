#include "state_machine.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "state_machine";
static system_state_t s_state = STATE_IDLE;
static system_health_t s_health;

const char *state_machine_state_name(system_state_t state)
{
    switch (state) {
    case STATE_IDLE: return "IDLE";
    case STATE_MONITORING: return "MONITORING";
    case STATE_ALERT: return "ALERT";
    case STATE_UPLOADING: return "UPLOADING";
    case STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

esp_err_t state_machine_init(void)
{
    s_state = STATE_IDLE;
    memset(&s_health, 0, sizeof(s_health));
    s_health.health_score = 100;
    ESP_LOGI(TAG, "状态机初始化完成");
    return ESP_OK;
}

system_state_t state_machine_get_state(void)
{
    return s_state;
}

void state_machine_update_health(const system_health_t *health)
{
    if (health == NULL) {
        return;
    }
    s_health = *health;
    uint8_t score = 0;
    if (health->sensor_ok) score += 25;
    if (health->eeg_ok) score += 20;
    if (health->camera_ok) score += 25;
    if (health->ble_ok) score += 15;
    if (health->wifi_ok) score += 15;
    s_health.health_score = score;

    if (score < 30) {
        state_machine_set_state(STATE_ERROR);
    }
}

void state_machine_set_state(system_state_t new_state)
{
    if (s_state == new_state) {
        return;
    }
    ESP_LOGI(TAG, "状态转换: %s -> %s health=%u",
             state_machine_state_name(s_state),
             state_machine_state_name(new_state),
             s_health.health_score);
    s_state = new_state;
}

const system_health_t *state_machine_get_health(void)
{
    return &s_health;
}
