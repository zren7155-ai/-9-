#include "temporal_learning.h"

#include <string.h>

#include "adaptive_learning.h"
#include "ai_feedback_parser.h"
#include "eeg.h"
#include "eeg_trend.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "learning_storage.h"
#include "pose_trend.h"
#include "risk_engine.h"
#include "sensor.h"

#define TEMPORAL_TASK_PERIOD_MS 200

static const char *TAG = "temporal_learning";

typedef struct {
    TaskHandle_t task_handle;
    temporal_learning_snapshot_t snapshot;
    uint8_t last_risk_pre;
    uint64_t risk_rise_start_ms;
    bool initialized;
} temporal_learning_ctx_t;

static temporal_learning_ctx_t s_ctx;

static uint8_t abs_u8_delta(uint8_t a, uint8_t b)
{
    return a > b ? (uint8_t)(a - b) : (uint8_t)(b - a);
}

static void update_risk_temporal_fields(const risk_sample_t *risk)
{
    uint64_t now = bodyguard_now_ms();
    s_ctx.snapshot.risk_velocity = abs_u8_delta(risk->risk_pre, s_ctx.last_risk_pre);
    s_ctx.snapshot.risk_rising = risk->risk_pre > s_ctx.last_risk_pre;

    if (risk->risk_pre >= BODYGUARD_RISK_TRIGGER_THRESHOLD) {
        if (s_ctx.risk_rise_start_ms == 0) {
            s_ctx.risk_rise_start_ms = now;
        }
        s_ctx.snapshot.risk_hold_ms = (uint32_t)(now - s_ctx.risk_rise_start_ms);
    } else {
        s_ctx.risk_rise_start_ms = 0;
        s_ctx.snapshot.risk_hold_ms = 0;
    }

    s_ctx.last_risk_pre = risk->risk_pre;
}

static void temporal_learning_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "temporal learning task started period=%dms", TEMPORAL_TASK_PERIOD_MS);

    while (true) {
        sensor_sample_t sensor = sensor_get_latest();
        eeg_sample_t eeg = eeg_get_latest();
        risk_sample_t risk = risk_engine_get_latest();

        pose_trend_push(&sensor);
        eeg_trend_push(&eeg);

        s_ctx.snapshot.pose = pose_trend_get();
        s_ctx.snapshot.eeg = eeg_trend_get();
        update_risk_temporal_fields(&risk);
        s_ctx.snapshot.timestamp_ms = bodyguard_now_ms();

        vTaskDelay(pdMS_TO_TICKS(TEMPORAL_TASK_PERIOD_MS));
    }
}

esp_err_t temporal_learning_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    eeg_trend_reset();
    pose_trend_reset();

    esp_err_t storage_err = learning_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "learning storage degraded: %s", esp_err_to_name(storage_err));
    }

    esp_err_t err = adaptive_learning_init();
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t temporal_learning_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(temporal_learning_task, "temporal_learning",
                                BODYGUARD_TASK_STACK_MEDIUM, NULL,
                                BODYGUARD_TASK_PRIO_WIFI, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

temporal_learning_snapshot_t temporal_learning_get_snapshot(void)
{
    return s_ctx.snapshot;
}

esp_err_t temporal_learning_on_ai_response(const char *event_id, const char *response)
{
    learning_ai_feedback_t feedback;
    esp_err_t err = ai_feedback_parser_parse(response, &feedback);
    if (err != ESP_OK) {
        return err;
    }

    if (event_id != NULL && event_id[0] != '\0' &&
        strncmp(event_id, feedback.event_id, sizeof(feedback.event_id)) != 0) {
        ESP_LOGW(TAG, "ignore AI feedback event mismatch local=%s remote=%s",
                 event_id, feedback.event_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return adaptive_learning_apply_feedback(&feedback);
}
