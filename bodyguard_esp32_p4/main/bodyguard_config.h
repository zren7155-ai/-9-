#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "bodyguard_private_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 工程身份 */
#define BODYGUARD_PROJECT_NAME                 "bodyguard_esp32_p4"
#define BODYGUARD_FIRMWARE_VERSION            "industrial-1.0.0"


/* 可视化 AI 运行调试模式：只增加日志与观测能力，不改变原有业务逻辑。 */
/* 云端 AI：ESP32-P4 只负责采集、初判和上传，不做任何本地 AI 推理。 */
#define BODYGUARD_AI_EVENT_UPLOAD_PATH         "/v1/ai/event_upload"
#define BODYGUARD_DATA_UPLOAD_PATH             "/v1/data"
#define BODYGUARD_AI_EVENT_UPLOAD_URL          BODYGUARD_CLOUD_HOST BODYGUARD_AI_EVENT_UPLOAD_PATH
#define BODYGUARD_DATA_UPLOAD_URL              BODYGUARD_CLOUD_HOST BODYGUARD_DATA_UPLOAD_PATH
#define BODYGUARD_HTTP_TIMEOUT_MS              10000

/* BrainLink Lite 与本机标识。ESP32-P4 的 BLE 控制器通常需要外置 HCI 控制器。 */
#define BODYGUARD_BRAINLINK_MAC                "90:E2:FC:2C:5C:D7"
#define BODYGUARD_ESP32P4_BLE_MAC              "58:E6:C5:CB:EF:DE"
#define BODYGUARD_EEG_SERVICE_UUID             "0000FFF0-0000-1000-8000-00805F9B34FB"
#define BODYGUARD_EEG_CHAR_UUID                "0000FFF4-0000-1000-8000-00805F9B34FB"
#define BODYGUARD_BLE_DEVICE_NAME              "BodyGuard-P4"

/* PC 网关 EEG 串口。PC 通过 USB-UART 将 BrainLink 数据桥接到 P4。 */
#define BODYGUARD_PC_EEG_UART_NUM              UART_NUM_2
#define BODYGUARD_PC_EEG_UART_BAUD             115200
#define BODYGUARD_PC_EEG_UART_TX_GPIO          GPIO_NUM_1
#define BODYGUARD_PC_EEG_UART_RX_GPIO          GPIO_NUM_2

/* MPU6050 I2C。实际板级引脚如有差异，只改这里。 */
#define BODYGUARD_I2C_PORT                     0
#define BODYGUARD_I2C_PORT_FREQ_HZ             100000
#define BODYGUARD_I2C_SDA_GPIO                 GPIO_NUM_7
#define BODYGUARD_I2C_SCL_GPIO                 GPIO_NUM_8
#define BODYGUARD_MPU6050_ADDR                 0x68

/* TF 卡 SPI 缓存 */
#define BODYGUARD_TF_SPI_CS_GPIO               GPIO_NUM_3
#define BODYGUARD_TF_SPI_SCK_GPIO              GPIO_NUM_46
#define BODYGUARD_TF_SPI_MISO_GPIO             GPIO_NUM_48
#define BODYGUARD_TF_SPI_MOSI_GPIO             GPIO_NUM_47
#define BODYGUARD_TF_SPI_MAX_FREQ_KHZ          10000
#define BODYGUARD_TF_MOUNT_POINT               "/sdcard"
#define BODYGUARD_EVENT_CACHE_DIR              BODYGUARD_TF_MOUNT_POINT "/events"

/* ESP32-P4-NANO 板载 ES8311 + NS4150B 音频链路 */
#define BODYGUARD_AUDIO_I2S_NUM                0
#define BODYGUARD_AUDIO_I2S_MCLK_GPIO          GPIO_NUM_13
#define BODYGUARD_AUDIO_I2S_BCLK_GPIO          GPIO_NUM_12
#define BODYGUARD_AUDIO_I2S_WS_GPIO            GPIO_NUM_10
#define BODYGUARD_AUDIO_I2S_DOUT_GPIO          GPIO_NUM_9
#define BODYGUARD_AUDIO_I2S_DIN_GPIO           GPIO_NUM_11
#define BODYGUARD_AUDIO_PA_EN_GPIO             GPIO_NUM_53
#define BODYGUARD_AUDIO_SAMPLE_RATE            16000
#define BODYGUARD_AUDIO_MCLK_MULTIPLE          384
#define BODYGUARD_AUDIO_VOLUME                 70
#define BODYGUARD_VOICE_COOLDOWN_MS            15000
#define BODYGUARD_VOICE_RESET_RISK             30
#define BODYGUARD_VOICE_RESET_HOLD_MS          300000
#define BODYGUARD_VOICE_WAV_ANALYZING          BODYGUARD_TF_MOUNT_POINT "/voice/analyzing.wav"
#define BODYGUARD_VOICE_WAV_FALL               BODYGUARD_TF_MOUNT_POINT "/voice/fall_detected.wav"
#define BODYGUARD_VOICE_WAV_FATIGUE            BODYGUARD_TF_MOUNT_POINT "/voice/fatigue.wav"

/* OV5647 MIPI 摄像头事件窗口。JPEG 数据放在 PSRAM 环形缓冲。 */
#define BODYGUARD_CAMERA_LOW_FPS               10
#define BODYGUARD_CAMERA_HIGH_FPS              30
#define BODYGUARD_EVENT_WINDOW_SECONDS         5
#define BODYGUARD_CAMERA_RING_FRAMES           (BODYGUARD_CAMERA_LOW_FPS * BODYGUARD_EVENT_WINDOW_SECONDS)
#define BODYGUARD_CAMERA_JPEG_SLOT_SIZE        (480 * 1024)
#define BODYGUARD_CAMERA_SNAPSHOT_MAX_SIZE     (900 * 1024)
#define BODYGUARD_CAMERA_RETRY_MS              1000
#define BODYGUARD_EVENT_MIN_VIDEO_FRAMES       8

/* 风险规则：只做轻量阈值初判，AI 结论必须由云端返回。 */
#define BODYGUARD_RISK_TRIGGER_THRESHOLD       70
#define BODYGUARD_EMERGENCY_THRESHOLD          90
#define BODYGUARD_EVENT_COOLDOWN_MS            60000
#define BODYGUARD_RISK_HOLD_MS                 5000
#define BODYGUARD_POSTURE_STABLE_MS            1500
#define BODYGUARD_EEG_DROP_HOLD_MS             3000
#define BODYGUARD_EEG_LOSS_MS                  10000
#define BODYGUARD_FALL_ANGLE_DEG               70.0f
#define BODYGUARD_FAST_ANGLE_DELTA_DEG         20.0f
#define BODYGUARD_EEG_SIGNAL_BAD_THRESHOLD     50
#define BODYGUARD_ACCEL_DIFF_SMALL             150
#define BODYGUARD_ACCEL_DIFF_MID               400
#define BODYGUARD_ACCEL_DIFF_HIGH              800
#define BODYGUARD_GYRO_DIFF_SMALL              80
#define BODYGUARD_GYRO_DIFF_MID                200
#define BODYGUARD_GYRO_DIFF_HIGH               400

#define BODYGUARD_AI_NORMAL                    1
#define BODYGUARD_AI_MONITOR                   2
#define BODYGUARD_AI_DANGER                    3

/* FreeRTOS 周期 */
#define BODYGUARD_SENSOR_POLL_MS               10
#define BODYGUARD_EEG_POLL_MS                  20
#define BODYGUARD_RISK_EVAL_MS                 50
#define BODYGUARD_EVENT_TASK_MS                3000
#define BODYGUARD_BLE_PERIOD_MS                100
#define BODYGUARD_WIFI_TASK_IDLE_MS            500

/* 串口遥测：通过 USB 串口把脑电、姿态和低帧率 JPEG 预览发送给电脑看板。 */
#define BODYGUARD_SERIAL_TELEMETRY_ENABLE      1
#define BODYGUARD_SERIAL_TELEMETRY_MS          50
#define BODYGUARD_SERIAL_CAMERA_ENABLE         0
#define BODYGUARD_SERIAL_CAMERA_MS             1000
#define BODYGUARD_SERIAL_CAMERA_MAX_BYTES      (96 * 1024)

/* WiFi MJPEG 实时视频流。串口只承载数据，视频走 HTTP，避免串口带宽拖慢画面。 */
#define BODYGUARD_MJPEG_STREAM_ENABLE          1
#define BODYGUARD_MJPEG_STREAM_PORT            8080
#define BODYGUARD_MJPEG_FRAME_MS               66
#define BODYGUARD_MJPEG_FRAME_MAX_BYTES        (480 * 1024)
#define BODYGUARD_MJPEG_BOUNDARY               "bodyguardframe"

/* 队列与重试 */
#define BODYGUARD_SENSOR_QUEUE_DEPTH           32
#define BODYGUARD_EEG_QUEUE_DEPTH              32
#define BODYGUARD_EVENT_QUEUE_DEPTH            8
#define BODYGUARD_EVENT_TRIGGER_QUEUE_DEPTH    4
#define BODYGUARD_EVENT_CAPTURE_QUEUE_DEPTH    4
#define BODYGUARD_EVENT_CAPTURE_MAX_IMAGES     3
#define BODYGUARD_UPLOAD_MAX_IMAGES            3
#define BODYGUARD_UPLOAD_MAX_RETRY             5
#define BODYGUARD_AI_HTTP_RETRY                2
#define BODYGUARD_UPLOAD_RETRY_INTERVALS_MS    5000, 30000, 60000, 120000, 300000

/* 任务资源 */
#define BODYGUARD_TASK_STACK_HUGE              8192
#define BODYGUARD_TASK_STACK_LARGE             6144
#define BODYGUARD_TASK_STACK_MEDIUM            4096
#define BODYGUARD_TASK_STACK_SMALL             3072
#define BODYGUARD_TASK_STACK_AUDIO             6144
#define BODYGUARD_TASK_STACK_TELEMETRY         6144
#define BODYGUARD_TASK_PRIO_SENSOR             7
#define BODYGUARD_TASK_PRIO_RISK               6
#define BODYGUARD_TASK_PRIO_EVENT              6
#define BODYGUARD_TASK_PRIO_EVENT_ROUTER       5
#define BODYGUARD_TASK_PRIO_CAMERA             5
#define BODYGUARD_TASK_PRIO_EVENT_CAPTURE      4
#define BODYGUARD_TASK_PRIO_EEG                5
#define BODYGUARD_TASK_PRIO_BLE                4
#define BODYGUARD_TASK_PRIO_WIFI               2

#define BODYGUARD_EVENT_ID_STR_LEN             32
#define BODYGUARD_NVS_NAMESPACE                "bg_guard"
#define BODYGUARD_NVS_EVENT_PREFIX             "ev"

#ifdef __cplusplus
}
#endif
