#include "learning_loop_engine.h"

#include <string.h>

#include "adaptive_learning.h"
#include "bodyguard_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "risk_engine.h"

#define LEARNING_HISTORY_SIZE 100
#define LEARNING_LOOP_PERIOD_MS 1000

static const char *TAG = "learning_loop";

typedef struct {
    risk_sample_t risk;
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint8_t ai_result;
    bool has_ai;
} learning_history_item_t;

typedef struct {
    learning_history_item_t history[LEARNING_HISTORY_SIZE];
    size_t write_index;
    size_t count;
    TaskHandle_t task;
    bool initialized;
    portMUX_TYPE lock;
} learning_loop_ctx_t;

static learning_loop_ctx_t s_ctx = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

esp_err_t learning_loop_engine_init(void)
{
    memset(&s_ctx.history, 0, sizeof(s_ctx.history));
    s_ctx.write_index = 0;
    s_ctx.count = 0;
    s_ctx.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_ctx.initialized = true;
    return ESP_OK;
}

void learning_loop_engine_record_risk(const risk_sample_t *risk)
{
    if (!s_ctx.initialized || risk == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_ctx.lock);
    learning_history_item_t *item = &s_ctx.history[s_ctx.write_index];
    memset(item, 0, sizeof(*item));
    item->risk = *risk;
    strlcpy(item->event_id, event_queue_current_event_id(), sizeof(item->event_id));
    s_ctx.write_index = (s_ctx.write_index + 1) % LEARNING_HISTORY_SIZE;
    if (s_ctx.count < LEARNING_HISTORY_SIZE) {
        s_ctx.count++;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);
}

void learning_loop_engine_record_ai(const char *event_id, uint8_t ai_result)
{
    if (!s_ctx.initialized || event_id == NULL || event_id[0] == '\0') {
        return;
    }

    taskENTER_CRITICAL(&s_ctx.lock);
    for (size_t i = 0; i < s_ctx.count; i++) {
        size_t idx = (s_ctx.write_index + LEARNING_HISTORY_SIZE - 1 - i) % LEARNING_HISTORY_SIZE;
        learning_history_item_t *item = &s_ctx.history[idx];
        if (strncmp(item->event_id, event_id, sizeof(item->event_id)) == 0) {
            item->ai_result = ai_result;
            item->has_ai = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_ctx.lock);
}

static bool newest_ai_sample(learning_history_item_t *out)
{
    bool found = false;
    taskENTER_CRITICAL(&s_ctx.lock);
    for (size_t i = 0; i < s_ctx.count; i++) {
        size_t idx = (s_ctx.write_index + LEARNING_HISTORY_SIZE - 1 - i) % LEARNING_HISTORY_SIZE;
        if (s_ctx.history[idx].has_ai) {
            *out = s_ctx.history[idx];
            s_ctx.history[idx].has_ai = false;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_ctx.lock);
    return found;
}

static void learning_loop_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "learning_loop_task started period=%dms", LEARNING_LOOP_PERIOD_MS);

    while (true) {
        learning_history_item_t item;
        if (newest_ai_sample(&item)) {
            (void)adaptive_learning_apply_closed_loop(item.risk.risk_pre,
                                                      item.ai_result,
                                                      item.risk.pose_score,
                                                      item.risk.eeg_score,
                                                      item.risk.burst_score,
                                                      item.event_id);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(LEARNING_LOOP_PERIOD_MS));
    }
}

esp_err_t learning_loop_engine_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "learning loop not initialized");
    if (s_ctx.task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(learning_loop_task, "learning_loop_task",
                                BODYGUARD_TASK_STACK_SMALL, NULL,
                                BODYGUARD_TASK_PRIO_WIFI, &s_ctx.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
