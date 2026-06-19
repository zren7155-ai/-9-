#include "pc_eeg_bridge.h"

#include <string.h>

#include "bodyguard_config.h"
#include "driver/uart.h"
#include "eeg.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"

static const char *TAG = "uart_eeg";

#define EEG_FRAME_HEAD 0xBB
#define EEG_FRAME_TAIL 0xEE
#define EEG_FRAME_LEN  5

typedef struct {
    TaskHandle_t task_handle;
    bool initialized;
    uint64_t last_valid_ms;
    uint64_t last_rx_log_ms;
    uint32_t rx_byte_count;
    uint32_t rx_discard_count;
    uint32_t rx_frame_error_count;
    uint8_t last_discard_byte;
    uint8_t last_bad_frame[EEG_FRAME_LEN];
} pc_bridge_ctx_t;

static pc_bridge_ctx_t s_ctx;

static inline uint8_t noise_to_quality(uint8_t noise)
{
    return noise >= 100 ? 0 : (uint8_t)(100 - noise);
}

static void handle_frame(const uint8_t frame[EEG_FRAME_LEN])
{
    if (frame[0] != EEG_FRAME_HEAD || frame[4] != EEG_FRAME_TAIL) {
        memcpy(s_ctx.last_bad_frame, frame, sizeof(s_ctx.last_bad_frame));
        s_ctx.rx_frame_error_count++;
        ESP_LOGW(TAG, "drop invalid EEG frame raw=%02X %02X %02X %02X %02X",
                 frame[0], frame[1], frame[2], frame[3], frame[4]);
        return;
    }

    uint8_t attention = frame[1];
    uint8_t fatigue = frame[2];
    uint8_t noise = frame[3];
    if (attention > 100 || fatigue > 100 || noise > 100) {
        ESP_LOGW(TAG, "drop out-of-range EEG frame attention=%u fatigue=%u noise=%u",
                 attention, fatigue, noise);
        return;
    }

    uint8_t quality = noise_to_quality(noise);
    uint8_t payload[3] = {attention, fatigue, quality};
    eeg_ingest_brainlink_payload(payload, sizeof(payload));
    s_ctx.last_valid_ms = bodyguard_now_ms();

    ESP_LOGI(TAG, "WROOM EEG attention=%u fatigue=%u noise=%u", attention, fatigue, noise);
}

static void pc_bridge_task(void *arg)
{
    (void)arg;
    uint8_t byte = 0;
    uint8_t frame[EEG_FRAME_LEN] = {0};
    size_t pos = 0;

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "uart_eeg_task started UART%d RX=%d TX=%d baud=%d",
             BODYGUARD_PC_EEG_UART_NUM,
             BODYGUARD_PC_EEG_UART_RX_GPIO,
             BODYGUARD_PC_EEG_UART_TX_GPIO,
             BODYGUARD_PC_EEG_UART_BAUD);

    while (true) {
        int n = uart_read_bytes(BODYGUARD_PC_EEG_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (n == 1) {
            s_ctx.rx_byte_count++;
            if (pos == 0 && byte != EEG_FRAME_HEAD) {
                s_ctx.rx_discard_count++;
                s_ctx.last_discard_byte = byte;
                continue;
            }
            frame[pos++] = byte;
            if (pos == EEG_FRAME_LEN) {
                handle_frame(frame);
                pos = 0;
            }
        }

        uint64_t now = bodyguard_now_ms();
        if (now - s_ctx.last_rx_log_ms >= 1000) {
            if (s_ctx.rx_byte_count == 0) {
                ESP_LOGW(TAG, "WROOM UART idle: no bytes on RX=%d in last 1s",
                         BODYGUARD_PC_EEG_UART_RX_GPIO);
            } else {
                ESP_LOGI(TAG,
                         "WROOM UART stats: bytes=%lu discards=%lu frame_errors=%lu last_discard=0x%02X",
                         (unsigned long)s_ctx.rx_byte_count,
                         (unsigned long)s_ctx.rx_discard_count,
                         (unsigned long)s_ctx.rx_frame_error_count,
                         s_ctx.last_discard_byte);
            }
            s_ctx.rx_byte_count = 0;
            s_ctx.rx_discard_count = 0;
            s_ctx.rx_frame_error_count = 0;
            s_ctx.last_discard_byte = 0;
            s_ctx.last_rx_log_ms = now;
        }

        if (s_ctx.last_valid_ms != 0 &&
            now - s_ctx.last_valid_ms > BODYGUARD_EEG_LOSS_MS) {
            eeg_set_connected(false);
            s_ctx.last_valid_ms = 0;
            ESP_LOGW(TAG, "EEG signal lost, no valid WROOM frame for %dms", BODYGUARD_EEG_LOSS_MS);
        }

        esp_task_wdt_reset();
    }
}

esp_err_t pc_eeg_bridge_init(void)
{
    uart_config_t cfg = {
        .baud_rate = BODYGUARD_PC_EEG_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BODYGUARD_PC_EEG_UART_NUM, 2048, 0, 0, NULL, 0),
                        TAG, "install uart driver failed");
    ESP_RETURN_ON_ERROR(uart_param_config(BODYGUARD_PC_EEG_UART_NUM, &cfg),
                        TAG, "config uart failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(BODYGUARD_PC_EEG_UART_NUM,
                                     BODYGUARD_PC_EEG_UART_TX_GPIO,
                                     BODYGUARD_PC_EEG_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set uart pin failed");

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t pc_eeg_bridge_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "bridge not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(pc_bridge_task, "uart_eeg_task", BODYGUARD_TASK_STACK_SMALL,
                                NULL, BODYGUARD_TASK_PRIO_EEG, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
