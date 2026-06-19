#include "event_router.h"

#include <string.h>

#include "audio_alert.h"
#include "bodyguard_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "event_capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "event_router";

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    uint64_t last_confirm_ms;
    uint64_t last_emergency_ms;
    bool initialized;
} event_router_ctx_t;

static event_router_ctx_t s_ctx;

esp_err_t event_router_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.queue = xQueueCreate(BODYGUARD_EVENT_TRIGGER_QUEUE_DEPTH, sizeof(event_trigger_t));
    ESP_RETURN_ON_FALSE(s_ctx.queue != NULL, ESP_ERR_NO_MEM, TAG, "trigger queue alloc failed");
    s_ctx.initialized = true;
    return ESP_OK;
}

bool event_router_post_trigger(const event_trigger_t *trigger)
{
    if (!s_ctx.initialized || s_ctx.queue == NULL || trigger == NULL) {
        return false;
    }

    event_trigger_t copy = *trigger;
    copy.timestamp_ms = copy.timestamp_ms ? copy.timestamp_ms : bodyguard_now_ms();
    if (xQueueSend(s_ctx.queue, &copy, 0) != pdTRUE) {
        event_trigger_t drop;
        (void)xQueueReceive(s_ctx.queue, &drop, 0);
        return xQueueSend(s_ctx.queue, &copy, 0) == pdTRUE;
    }
    return true;
}

static void event_router_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "event_router_task started");

    while (true) {
        event_trigger_t trigger;
        if (xQueueReceive(s_ctx.queue, &trigger, pdMS_TO_TICKS(500)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }

        if (trigger.type == EVENT_TRIGGER_WARNING) {
            audio_alert_on_warning(trigger.risk.risk_final);
            esp_task_wdt_reset();
            continue;
        }

        if (trigger.type == EVENT_TRIGGER_CONFIRM ||
            trigger.type == EVENT_TRIGGER_EMERGENCY) {
            uint64_t now = bodyguard_now_ms();
            if (trigger.type == EVENT_TRIGGER_CONFIRM) {
                if (s_ctx.last_confirm_ms != 0 &&
                    now - s_ctx.last_confirm_ms < BODYGUARD_EVENT_COOLDOWN_MS) {
                    ESP_LOGW(TAG, "drop duplicate confirm trigger risk=%u", trigger.risk.risk_final);
                    esp_task_wdt_reset();
                    continue;
                }
                s_ctx.last_confirm_ms = now;
            } else {
                if (s_ctx.last_emergency_ms != 0 &&
                    now - s_ctx.last_emergency_ms < BODYGUARD_EVENT_COOLDOWN_MS) {
                    ESP_LOGW(TAG, "drop duplicate emergency trigger risk=%u", trigger.risk.risk_final);
                    esp_task_wdt_reset();
                    continue;
                }
                s_ctx.last_emergency_ms = now;
            }

            if (!event_capture_post(&trigger)) {
                ESP_LOGW(TAG, "event capture queue full risk=%u", trigger.risk.risk_final);
            }
        }

        esp_task_wdt_reset();
    }
}

esp_err_t event_router_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "event_router not initialized");
    if (s_ctx.task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(event_router_task, "event_router_task",
                                BODYGUARD_TASK_STACK_SMALL, NULL,
                                BODYGUARD_TASK_PRIO_EVENT_ROUTER, &s_ctx.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
