#include "eeg.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"

static const char *TAG = "eeg";

typedef struct {
    QueueHandle_t output_queue;
    TaskHandle_t task_handle;
    eeg_sample_t latest;
    bool connected;
    bool filter_ready;
    bool initialized;
    portMUX_TYPE lock;
} eeg_ctx_t;

static eeg_ctx_t s_ctx = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint8_t smooth_u8(uint8_t old_value, uint8_t new_value)
{
    return (uint8_t)(((uint16_t)old_value * 3U + new_value + 2U) / 4U);
}

static void stabilize_sample(eeg_sample_t *sample)
{
    if (sample == NULL || !sample->connected) {
        return;
    }

    eeg_sample_t previous;
    bool ready;
    taskENTER_CRITICAL(&s_ctx.lock);
    previous = s_ctx.latest;
    ready = s_ctx.filter_ready && s_ctx.connected;
    if (!s_ctx.filter_ready) {
        s_ctx.filter_ready = true;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (!ready) {
        return;
    }

    sample->attention = smooth_u8(previous.attention, sample->attention);
    sample->fatigue = smooth_u8(previous.fatigue, sample->fatigue);
    sample->relax = smooth_u8(previous.relax, sample->relax);
    sample->signal_quality = smooth_u8(previous.signal_quality, sample->signal_quality);
}

static bool parse_brainlink_payload(const uint8_t *data, uint16_t len, eeg_sample_t *out)
{
    if (data == NULL || out == NULL || len == 0) {
        return false;
    }

    eeg_sample_t sample;
    taskENTER_CRITICAL(&s_ctx.lock);
    sample = s_ctx.latest;
    taskEXIT_CRITICAL(&s_ctx.lock);

    sample.connected = true;
    sample.timestamp_ms = bodyguard_now_ms();

    if (len >= 4) {
        sample.attention = data[0];
        sample.fatigue = data[1];
        sample.relax = data[2];
        sample.signal_quality = data[3];
    } else if (len >= 3) {
        sample.attention = data[0];
        sample.fatigue = data[1];
        sample.signal_quality = data[2];
        sample.relax = sample.fatigue <= 100 ? (uint8_t)(100 - sample.fatigue) : 0;
    } else if (len >= 2) {
        sample.attention = data[0];
        sample.fatigue = data[1];
        sample.relax = sample.fatigue <= 100 ? (uint8_t)(100 - sample.fatigue) : 0;
        sample.signal_quality = 80;
    } else {
        sample.attention = data[0];
        sample.fatigue = data[0] > 60 ? (uint8_t)(data[0] - 40) : 0;
        sample.relax = data[0] < 100 ? (uint8_t)(100 - data[0]) : 0;
        sample.signal_quality = 50;
    }

    if (sample.signal_quality < BODYGUARD_EEG_SIGNAL_BAD_THRESHOLD) {
        taskENTER_CRITICAL(&s_ctx.lock);
        sample.attention = s_ctx.latest.attention;
        sample.fatigue = s_ctx.latest.fatigue;
        sample.relax = s_ctx.latest.relax;
        taskEXIT_CRITICAL(&s_ctx.lock);
        ESP_LOGW(TAG, "BrainLink 信号质量差，沿用上一帧数据 quality=%u", sample.signal_quality);
    }

    *out = sample;
    return true;
}

static void push_eeg_sample(const eeg_sample_t *sample)
{
    taskENTER_CRITICAL(&s_ctx.lock);
    s_ctx.latest = *sample;
    s_ctx.connected = sample->connected;
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (xQueueSend(s_ctx.output_queue, sample, 0) != pdTRUE) {
        eeg_sample_t drop;
        xQueueReceive(s_ctx.output_queue, &drop, 0);
        xQueueSend(s_ctx.output_queue, sample, 0);
    }
}

static void eeg_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "task_eeg 启动，目标 BrainLink MAC=%s", BODYGUARD_BRAINLINK_MAC);

    while (true) {
        eeg_sample_t sample;
        taskENTER_CRITICAL(&s_ctx.lock);
        sample = s_ctx.latest;
        sample.connected = s_ctx.connected;
        taskEXIT_CRITICAL(&s_ctx.lock);

        sample.timestamp_ms = bodyguard_now_ms();
        if (!sample.connected) {
            sample.signal_quality = 0;
        }
        push_eeg_sample(&sample);

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_EEG_POLL_MS));
    }
}

esp_err_t eeg_init(QueueHandle_t output_queue)
{
    ESP_RETURN_ON_FALSE(output_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "eeg queue 为空");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.output_queue = output_queue;
    s_ctx.latest.attention = 50;
    s_ctx.latest.fatigue = 0;
    s_ctx.latest.relax = 50;
    s_ctx.latest.signal_quality = 0;
    s_ctx.latest.connected = false;
    s_ctx.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "EEG 模块初始化完成，目标 BrainLink=%s", BODYGUARD_BRAINLINK_MAC);
    return ESP_OK;
}

esp_err_t eeg_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "eeg 未初始化");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(eeg_task, "task_eeg", BODYGUARD_TASK_STACK_MEDIUM, NULL,
                                BODYGUARD_TASK_PRIO_EEG, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void eeg_set_connected(bool connected)
{
    taskENTER_CRITICAL(&s_ctx.lock);
    s_ctx.connected = connected;
    s_ctx.latest.connected = connected;
    s_ctx.latest.timestamp_ms = bodyguard_now_ms();
    if (!connected) {
        s_ctx.latest.signal_quality = 0;
        s_ctx.filter_ready = false;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);
}

void eeg_ingest_brainlink_payload(const uint8_t *data, uint16_t len)
{
    eeg_sample_t sample;
    if (!parse_brainlink_payload(data, len, &sample)) {
        return;
    }
    stabilize_sample(&sample);
    eeg_set_connected(true);
    push_eeg_sample(&sample);
}

void eeg_ingest_pc_bridge(uint8_t attention, uint8_t meditation, uint8_t signal)
{
    eeg_sample_t sample;

    taskENTER_CRITICAL(&s_ctx.lock);
    sample = s_ctx.latest;
    taskEXIT_CRITICAL(&s_ctx.lock);

    sample.attention = attention;
    sample.relax = meditation;
    sample.fatigue = meditation <= 100 ? (uint8_t)(100 - meditation) : 0;
    sample.signal_quality = signal;
    sample.connected = true;
    sample.timestamp_ms = bodyguard_now_ms();
    stabilize_sample(&sample);
    eeg_set_connected(true);
    push_eeg_sample(&sample);
}

bool eeg_is_connected(void)
{
    bool connected;
    taskENTER_CRITICAL(&s_ctx.lock);
    connected = s_ctx.connected;
    taskEXIT_CRITICAL(&s_ctx.lock);
    return connected;
}

eeg_sample_t eeg_get_latest(void)
{
    eeg_sample_t sample;
    taskENTER_CRITICAL(&s_ctx.lock);
    sample = s_ctx.latest;
    taskEXIT_CRITICAL(&s_ctx.lock);
    return sample;
}
