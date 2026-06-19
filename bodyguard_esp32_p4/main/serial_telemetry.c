#include "serial_telemetry.h"

#include <stdio.h>
#include <string.h>

#include "bodyguard_config.h"
#include "camera_ring.h"
#include "eeg.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "mbedtls/base64.h"
#include "risk_engine.h"
#include "sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_telemetry";

typedef struct {
    TaskHandle_t task;
    bool initialized;
    uint8_t *jpeg_buf;
    uint8_t *b64_buf;
} serial_telemetry_ctx_t;

static serial_telemetry_ctx_t s_ctx;

static void send_sensor_line(void)
{
    sensor_sample_t sensor = sensor_get_latest();
    eeg_sample_t eeg = eeg_get_latest();
    risk_sample_t risk = risk_engine_get_latest();
    uint8_t ai_report_status = risk.ai_report[0] ? 1 : 0;

    printf("BGDATA,{\"ts\":%llu,\"eeg\":{\"attention\":%u,\"meditation\":%u,"
           "\"fatigue\":%u,\"signal\":%u,\"connected\":%u},"
           "\"mpu\":{\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
           "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
           "\"angle\":%.2f,\"angle_delta\":%.2f,\"posture\":%u},"
           "\"risk\":{\"pre\":%u,\"final\":%u,\"confidence\":%u,\"eeg_state\":%u,"
           "\"pose_state\":%u,\"sys_state\":%u,\"ai_result\":%u,"
           "\"ai_synced\":%u,\"ai_report_status\":%u}}\n",
           (unsigned long long)bodyguard_now_ms(),
           eeg.attention,
           eeg.relax,
           eeg.fatigue,
           eeg.signal_quality,
           eeg.connected ? 1 : 0,
           sensor.accel[0],
           sensor.accel[1],
           sensor.accel[2],
           sensor.gyro[0],
           sensor.gyro[1],
           sensor.gyro[2],
           sensor.tilt_angle,
           sensor.angle_delta,
           (unsigned)sensor.posture,
           risk.risk_pre,
           risk.risk_final,
           risk.confidence,
           risk.eeg_state,
           risk.pose_state,
           risk.sys_state,
           risk.ai_result,
           risk.ai_synced ? 1 : 0,
           ai_report_status);
}

static void send_camera_line(void)
{
    size_t jpeg_size = 0;
    uint64_t frame_ts = 0;
    esp_err_t err = camera_ring_copy_latest_jpeg(s_ctx.jpeg_buf,
                                                 BODYGUARD_SERIAL_CAMERA_MAX_BYTES,
                                                 &jpeg_size,
                                                 &frame_ts);
    if (err != ESP_OK) {
        printf("BGERR,{\"source\":\"camera\",\"err\":\"%s\"}\n", esp_err_to_name(err));
        return;
    }

    size_t out_len = 0;
    size_t b64_cap = ((jpeg_size + 2) / 3) * 4 + 1;
    if (b64_cap > ((BODYGUARD_SERIAL_CAMERA_MAX_BYTES + 2) / 3) * 4 + 1) {
        printf("BGERR,{\"source\":\"camera\",\"err\":\"base64_overflow\"}\n");
        return;
    }

    err = mbedtls_base64_encode(s_ctx.b64_buf, b64_cap, &out_len, s_ctx.jpeg_buf, jpeg_size);
    if (err != 0) {
        printf("BGERR,{\"source\":\"camera\",\"err\":\"base64_%d\"}\n", err);
        return;
    }
    s_ctx.b64_buf[out_len] = '\0';

    printf("BGCAM,{\"ts\":%llu,\"size\":%u,\"jpeg_b64\":\"%s\"}\n",
           (unsigned long long)frame_ts,
           (unsigned)jpeg_size,
           (const char *)s_ctx.b64_buf);
}

static void telemetry_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint64_t last_camera_ms = 0;

    ESP_LOGI(TAG, "serial telemetry started: data=%dms camera=%dms",
             BODYGUARD_SERIAL_TELEMETRY_MS, BODYGUARD_SERIAL_CAMERA_MS);

    while (true) {
        uint64_t now = bodyguard_now_ms();
        send_sensor_line();
        if (BODYGUARD_SERIAL_CAMERA_ENABLE && now - last_camera_ms >= BODYGUARD_SERIAL_CAMERA_MS) {
            send_camera_line();
            last_camera_ms = now;
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_SERIAL_TELEMETRY_MS));
    }
}

esp_err_t serial_telemetry_init(void)
{
#if !BODYGUARD_SERIAL_TELEMETRY_ENABLE
    return ESP_OK;
#endif
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.jpeg_buf = heap_caps_malloc(BODYGUARD_SERIAL_CAMERA_MAX_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ctx.jpeg_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t b64_size = ((BODYGUARD_SERIAL_CAMERA_MAX_BYTES + 2) / 3) * 4 + 1;
    s_ctx.b64_buf = heap_caps_malloc(b64_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ctx.b64_buf == NULL) {
        heap_caps_free(s_ctx.jpeg_buf);
        s_ctx.jpeg_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t serial_telemetry_start(void)
{
#if !BODYGUARD_SERIAL_TELEMETRY_ENABLE
    return ESP_OK;
#endif
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "telemetry not initialized");
    if (s_ctx.task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(telemetry_task, "serial_telemetry",
                                BODYGUARD_TASK_STACK_TELEMETRY, NULL,
                                BODYGUARD_TASK_PRIO_WIFI, &s_ctx.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
