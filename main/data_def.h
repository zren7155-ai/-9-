/**
 * @file data_def.h
 * @brief Data definitions for the T5 safety monitor display terminal.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#ifndef __DATA_DEF_H__
#define __DATA_DEF_H__

#include "tuya_cloud_types.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_base_config.h"
#include "ai_audio_player.h"
#include "board_com_api.h"
#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tdl_led_manage.h"

#include "lvgl.h"
#include "lv_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define BLE_HEAD                         0xAA55
#define BLE_TAIL                         0x55AA
#define EVENT_ID_LEN                     32
#define AI_REPORT_LEN                    128
#define BODYGUARD_ADV_NAME               "BodyGuard-P4"
#define BODYGUARD_SERVICE_UUID           0xFD50
#define BODYGUARD_NOTIFY_CHAR_UUID       0x0002
#define BODYGUARD_NOTIFY_CHAR_UUID_STR   "00000002-0000-1001-8001-00805f9b07d0"
#define BODYGUARD_LEGACY_PACKET_SIZE     170
#define BODYGUARD_LEARNING_V1_PACKET_SIZE 201
#define BODYGUARD_PACKET_SIZE            207

#define T5_BLE_TIMEOUT_MS                5000
#define T5_BLE_RESCAN_DELAY_MS           3000
#define T5_UI_TASK_PRIO                  5
#define T5_UI_TASK_STACK_SIZE            (1024 * 8)
#define T5_ALERT_TASK_PRIO               4
#define T5_ALERT_TASK_STACK_SIZE         (1024 * 3)
#define T5_BLE_TASK_PRIO                 4
#define T5_BLE_TASK_STACK_SIZE           (1024 * 4)

#define T5_LOG_I(fmt, ...) PR_INFO("[bodyguard-t5] " fmt, ##__VA_ARGS__)
#define T5_LOG_E(fmt, ...) PR_ERR("[bodyguard-t5] " fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    T5_SYS_STANDBY = 0,
    T5_SYS_NORMAL = 1,
    T5_SYS_PRE_ALERT = 2,
    T5_SYS_EMERGENCY = 3,
} T5_SYS_STATE_E;

#pragma pack(push, 1)
typedef struct {
    uint16_t head;
    uint8_t sys_state;
    uint8_t risk_score;
    uint8_t eeg_state;
    uint8_t pose_state;
    char ai_report[AI_REPORT_LEN];
    uint8_t ble_conn_state;
    uint8_t cloud_conn_state;
    char event_id[EVENT_ID_LEN];
    float posture_weight;
    float eeg_weight;
    float burst_weight;
    uint8_t warning_threshold;
    uint8_t danger_threshold;
    uint32_t false_alarm_count;
    uint32_t confirmed_danger_count;
    uint32_t missed_danger_count;
    uint32_t learning_update_count;
    uint8_t learning_enabled;
    uint8_t eeg_attention;
    uint8_t eeg_meditation;
    uint8_t eeg_fatigue;
    uint8_t eeg_signal_quality;
    uint8_t eeg_connected;
    uint8_t eeg_score;
    uint16_t tail;
} ble_t5_packet_t;
#pragma pack(pop)

typedef struct {
    ble_t5_packet_t packet;
    bool packet_valid;
    bool ble_connected;
    bool ble_scanning;
    bool ble_connecting;
    bool learning_valid;
    uint32_t packet_seq;
    uint32_t last_packet_ms;
} t5_app_state_t;

typedef struct {
    bool ready;
    bool scanning;
    bool connecting;
    bool connected;
    uint16_t conn_handle;
    uint16_t notify_handle;
    uint8_t retry_count;
} t5_ble_status_t;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize shared application state.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_state_init(void);

/**
 * @brief Publish a valid packet into shared state.
 * @param[in] packet Packet received from ESP32-P4.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_state_update_packet(const ble_t5_packet_t *packet);

/**
 * @brief Publish a valid packet and mark whether learning fields are present.
 * @param[in] packet Packet received from ESP32-P4.
 * @param[in] learning_valid Whether the packet contains learning fields.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_state_update_packet_ex(const ble_t5_packet_t *packet, bool learning_valid);

/**
 * @brief Update BLE connection flags in shared state.
 * @param[in] connected Whether BLE is connected.
 * @param[in] scanning Whether BLE is scanning.
 * @param[in] connecting Whether BLE is connecting.
 * @return none
 */
void app_state_set_ble_status(bool connected, bool scanning, bool connecting);

/**
 * @brief Copy current shared state.
 * @param[out] state State snapshot.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET app_state_get(t5_app_state_t *state);

/**
 * @brief Decode and validate one BLE packet.
 * @param[in] data Raw bytes.
 * @param[in] len Raw byte count.
 * @param[out] packet Decoded packet.
 * @return true if packet is valid, otherwise false.
 */
bool app_state_decode_packet(const uint8_t *data, size_t len, ble_t5_packet_t *packet);

/**
 * @brief Initialize BLE client module.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET ble_client_init(void);

/**
 * @brief Start BLE scan/connect flow.
 * @return none
 */
void ble_client_start(void);

/**
 * @brief Force BLE rescan.
 * @return none
 */
void ble_client_restart(void);

/**
 * @brief Copy BLE diagnostic status.
 * @param[out] status BLE status snapshot.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET ble_client_get_status(t5_ble_status_t *status);

/**
 * @brief Initialize LVGL UI.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET ui_init(void);

/**
 * @brief Refresh UI from shared state.
 * @return none
 */
void ui_refresh(void);

/**
 * @brief Initialize local alert service.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET alert_init(void);

/**
 * @brief Apply current local alert state.
 * @return none
 */
void alert_refresh(void);

/**
 * @brief Main app entry.
 * @return none
 */
void bodyguard_t5_app_run(void);

/**
 * @brief Initialize application log system.
 * @return none
 */
void app_log_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __DATA_DEF_H__ */
