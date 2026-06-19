#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HEAD        0xAA55
#define BLE_TAIL        0x55AA
#define EVENT_ID_LEN    32
#define AI_REPORT_LEN   128

typedef struct __attribute__((packed)) {
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

#ifdef __cplusplus
}
#endif
