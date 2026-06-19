#include "ble.h"
#include "audio_alert.h"
#include "bodyguard_config.h"
#include "camera_ring.h"
#include "eeg.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "event_capture.h"
#include "event_queue.h"
#include "event_router.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mjpeg_stream.h"
#include "nvs_flash.h"
#include "pc_eeg_bridge.h"
#include "risk_engine.h"
#include "learning_loop_engine.h"
#include "serial_telemetry.h"
#include "sensor.h"
#include "state_machine.h"
#include "temporal_learning.h"
#include "wifi_upload.h"

static const char *TAG = "main";

static void fatal_restart(const char *module, esp_err_t err)
{
    ESP_LOGE(TAG, "%s 初始化失败: %s，3 秒后重启", module, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static void init_nvs_or_restart(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        fatal_restart("nvs_flash", ret);
    }
}

static void init_watchdog(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 15000,
        .idle_core_mask = 0x03,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fatal_restart("task_wdt", err);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "BodyGuard ESP32-P4 NANO 医疗级多模态安全监护系统");
    ESP_LOGI(TAG, "FW=%s BrainLink=%s LocalBLE=%s", BODYGUARD_FIRMWARE_VERSION,
             BODYGUARD_BRAINLINK_MAC, BODYGUARD_ESP32P4_BLE_MAC);
    ESP_LOGI(TAG, "云端 AI=%s", BODYGUARD_CLOUD_HOST);
    ESP_LOGI(TAG, "================================================");

    init_nvs_or_restart();
    init_watchdog();

    QueueHandle_t sensor_queue = xQueueCreate(BODYGUARD_SENSOR_QUEUE_DEPTH, sizeof(sensor_sample_t));
    QueueHandle_t eeg_queue = xQueueCreate(BODYGUARD_EEG_QUEUE_DEPTH, sizeof(eeg_sample_t));
    if (sensor_queue == NULL || eeg_queue == NULL) {
        fatal_restart("queue_create", ESP_ERR_NO_MEM);
    }

    esp_err_t ret = state_machine_init();
    if (ret != ESP_OK) fatal_restart("state_machine", ret);
    ret = event_queue_init();
    if (ret != ESP_OK) fatal_restart("event_queue", ret);
    ret = event_capture_init();
    if (ret != ESP_OK) fatal_restart("event_capture", ret);
    ret = event_router_init();
    if (ret != ESP_OK) fatal_restart("event_router", ret);
    ret = sensor_init(sensor_queue);
    if (ret != ESP_OK) fatal_restart("sensor", ret);
    ret = camera_ring_init_with_i2c(sensor_get_i2c_bus());
    if (ret != ESP_OK) fatal_restart("camera_ring", ret);
    ret = eeg_init(eeg_queue);
    if (ret != ESP_OK) fatal_restart("eeg", ret);
    ret = pc_eeg_bridge_init();
    if (ret != ESP_OK) fatal_restart("pc_eeg_bridge", ret);
    ret = risk_engine_init(sensor_queue, eeg_queue);
    if (ret != ESP_OK) fatal_restart("risk_engine", ret);
    ret = temporal_learning_init();
    if (ret != ESP_OK) fatal_restart("temporal_learning", ret);
    ret = learning_loop_engine_init();
    if (ret != ESP_OK) fatal_restart("learning_loop", ret);
    ret = wifi_upload_init();
    if (ret != ESP_OK) fatal_restart("wifi_upload", ret);
    ret = mjpeg_stream_init();
    if (ret != ESP_OK) fatal_restart("mjpeg_stream", ret);
    ret = event_queue_mount_storage();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "event storage mount degraded to NVS only: %s", esp_err_to_name(ret));
    }
    ret = ble_init();
    if (ret != ESP_OK) fatal_restart("ble", ret);
    ret = audio_alert_init();
    if (ret != ESP_OK) fatal_restart("audio_alert", ret);
    ret = serial_telemetry_init();
    if (ret != ESP_OK) fatal_restart("serial_telemetry", ret);

    if (sensor_start() != ESP_OK) fatal_restart("task_sensor", ESP_FAIL);
    if (camera_ring_start() != ESP_OK) fatal_restart("task_camera", ESP_FAIL);
    if (eeg_start() != ESP_OK) fatal_restart("task_eeg", ESP_FAIL);
    if (pc_eeg_bridge_start() != ESP_OK) fatal_restart("task_pc_eeg", ESP_FAIL);
    if (risk_engine_start() != ESP_OK) fatal_restart("task_risk", ESP_FAIL);
    if (event_capture_start() != ESP_OK) fatal_restart("task_event_capture", ESP_FAIL);
    if (event_router_start() != ESP_OK) fatal_restart("task_event_router", ESP_FAIL);
    if (temporal_learning_start() != ESP_OK) fatal_restart("task_temporal_learning", ESP_FAIL);
    if (learning_loop_engine_start() != ESP_OK) fatal_restart("task_learning_loop", ESP_FAIL);
    if (wifi_upload_start() != ESP_OK) fatal_restart("task_wifi_upload", ESP_FAIL);
    if (mjpeg_stream_start() != ESP_OK) fatal_restart("mjpeg_stream", ESP_FAIL);
    if (ble_start() != ESP_OK) fatal_restart("task_ble", ESP_FAIL);
    if (audio_alert_start() != ESP_OK) fatal_restart("task_audio", ESP_FAIL);
    if (serial_telemetry_start() != ESP_OK) fatal_restart("task_serial_telemetry", ESP_FAIL);
    system_health_t health = {
        .sensor_ok = true,
        .eeg_ok = true,
        .camera_ok = camera_ring_is_ready(),
        .ble_ok = ble_is_ready(),
        .wifi_ok = wifi_upload_is_connected(),
    };
    state_machine_update_health(&health);
    state_machine_set_state(STATE_MONITORING);
    ESP_LOGI(TAG, "所有任务已启动：sensor/eeg/risk/event/camera/ble/wifi_upload");
}
