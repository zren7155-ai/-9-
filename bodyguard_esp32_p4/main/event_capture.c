#include "event_capture.h"

#include <stdlib.h>
#include <string.h>

#include "audio_alert.h"
#include "bodyguard_config.h"
#include "camera_ring.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "event_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "risk_engine.h"
#include "state_machine.h"

static const char *TAG = "event_capture";

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    bool initialized;
} event_capture_ctx_t;

static event_capture_ctx_t s_ctx;

esp_err_t event_capture_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.queue = xQueueCreate(BODYGUARD_EVENT_CAPTURE_QUEUE_DEPTH, sizeof(event_trigger_t));
    ESP_RETURN_ON_FALSE(s_ctx.queue != NULL, ESP_ERR_NO_MEM, TAG, "capture queue alloc failed");
    s_ctx.initialized = true;
    return ESP_OK;
}

bool event_capture_post(const event_trigger_t *trigger)
{
    if (!s_ctx.initialized || s_ctx.queue == NULL || trigger == NULL) {
        return false;
    }

    event_trigger_t copy = *trigger;
    if (xQueueSend(s_ctx.queue, &copy, 0) != pdTRUE) {
        event_trigger_t drop;
        (void)xQueueReceive(s_ctx.queue, &drop, 0);
        return xQueueSend(s_ctx.queue, &copy, 0) == pdTRUE;
    }
    return true;
}

static bodyguard_event_t *allocate_event(void)
{
    bodyguard_event_t *event = heap_caps_calloc(1, sizeof(bodyguard_event_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event == NULL) {
        event = calloc(1, sizeof(bodyguard_event_t));
    }
    return event;
}

static bool clone_snapshot_from_last_frame(event_window_t *window)
{
    if (window == NULL || window->image_count == 0) {
        return true;
    }

    camera_frame_t *src = &window->images[window->image_count - 1];
    if (!src->valid || src->data == NULL || src->size == 0) {
        return true;
    }

    window->snapshot.data = heap_caps_malloc(src->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (window->snapshot.data == NULL) {
        window->snapshot.data = malloc(src->size);
    }
    if (window->snapshot.data == NULL) {
        memset(&window->snapshot, 0, sizeof(window->snapshot));
        return false;
    }

    memcpy(window->snapshot.data, src->data, src->size);
    window->snapshot.size = src->size;
    window->snapshot.timestamp_ms = src->timestamp_ms;
    window->snapshot.valid = true;
    window->snapshot.owned = true;
    return true;
}

static void event_capture_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "event_capture_task started");

    while (true) {
        event_trigger_t trigger;
        if (xQueueReceive(s_ctx.queue, &trigger, pdMS_TO_TICKS(500)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }

        bodyguard_event_t *event = allocate_event();
        if (event == NULL) {
            ESP_LOGE(TAG, "event allocation failed");
            esp_task_wdt_reset();
            continue;
        }

        event_id_generate(event->node.event_id, sizeof(event->node.event_id));
        event_queue_set_current_event_id(event->node.event_id);
        event->trigger = trigger.risk;
        risk_engine_bind_event_id(event->node.event_id);

        if (trigger.type == EVENT_TRIGGER_EMERGENCY) {
            audio_alert_on_emergency_event(event->node.event_id);
        } else {
            audio_alert_on_risk_event(event->node.event_id, event->trigger.risk_pre);
        }

        esp_err_t err = camera_ring_export_event_window(&event->window);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "camera event window export failed: %s", esp_err_to_name(err));
        }
        if (!clone_snapshot_from_last_frame(&event->window)) {
            ESP_LOGW(TAG, "snapshot clone skipped, PSRAM low event_id=%s", event->node.event_id);
        }
        risk_engine_export_event_series(&event->window);

        if (event_queue_push(event) != ESP_OK) {
            ESP_LOGE(TAG, "event queue push failed event_id=%s", event->node.event_id);
        }
        state_machine_set_state(event->trigger.risk_final >= BODYGUARD_EMERGENCY_THRESHOLD ?
                                STATE_ALERT : STATE_UPLOADING);
        ESP_LOGW(TAG, "event captured event_id=%s risk_pre=%u risk_final=%u frames=%u",
                 event->node.event_id, event->trigger.risk_pre, event->trigger.risk_final,
                 (unsigned)event->window.image_count);

        event_queue_release_payload(event);
        free(event);
        esp_task_wdt_reset();
    }
}

esp_err_t event_capture_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "event_capture not initialized");
    if (s_ctx.task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(event_capture_task, "event_capture_task",
                                BODYGUARD_TASK_STACK_HUGE, NULL,
                                BODYGUARD_TASK_PRIO_EVENT_CAPTURE, &s_ctx.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
