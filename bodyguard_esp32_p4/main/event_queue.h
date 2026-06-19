#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bodyguard_config.h"
#include "data_def.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POSTURE_NORMAL = 0,
    POSTURE_TILT = 1,
    POSTURE_FAST_CHANGE = 2,
    POSTURE_FALL = 3,
} posture_state_t;

typedef struct {
    float accel[3];
    float gyro[3];
    float tilt_angle;
    float angle_delta;
    posture_state_t posture;
    uint64_t timestamp_ms;
} sensor_sample_t;

typedef struct {
    uint8_t attention;
    uint8_t fatigue;
    uint8_t relax;
    uint8_t signal_quality;
    bool connected;
    uint64_t timestamp_ms;
} eeg_sample_t;

typedef struct {
    uint8_t pose_score;
    uint8_t eeg_score;
    uint8_t burst_score;
    uint8_t confidence;
    uint8_t risk_pre;
    uint8_t risk_final;
    uint8_t eeg_state;
    uint8_t pose_state;
    uint8_t sys_state;
    uint8_t ai_result;
    bool ai_synced;
    char ai_report[AI_REPORT_LEN];
    posture_state_t posture;
    int16_t gyro[3];
    uint64_t timestamp_ms;
} risk_sample_t;

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t timestamp_ms;
    bool valid;
    bool owned;
} camera_frame_t;

typedef struct {
    sensor_sample_t sensor[BODYGUARD_CAMERA_RING_FRAMES];
    size_t sensor_count;
    eeg_sample_t eeg[BODYGUARD_CAMERA_RING_FRAMES];
    size_t eeg_count;
    risk_sample_t risk[BODYGUARD_CAMERA_RING_FRAMES];
    size_t risk_count;
    camera_frame_t images[BODYGUARD_CAMERA_RING_FRAMES];
    size_t image_count;
    camera_frame_t snapshot;
} event_window_t;

typedef struct {
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint8_t image_status;
    uint8_t json_status;
    uint8_t ble_status;
    uint8_t retry_count;
} event_node_t;

typedef struct {
    event_node_t node;
    risk_sample_t trigger;
    event_window_t window;
    bool cached;
} bodyguard_event_t;

typedef struct __attribute__((packed)) {
    uint16_t head;
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint8_t risk_pre;
    uint8_t eeg;
    uint8_t posture;
    int16_t gyro[3];
    uint8_t checksum;
} ble_packet_t;

uint64_t bodyguard_now_ms(void);
void event_id_generate(char *buf, size_t buf_len);
uint8_t ble_packet_checksum(const ble_packet_t *packet);

esp_err_t event_queue_init(void);
esp_err_t event_queue_mount_storage(void);
esp_err_t event_queue_push(const bodyguard_event_t *event);
bool event_queue_pop_for_upload(bodyguard_event_t *event, uint32_t timeout_ms);
void event_queue_update_status(const event_node_t *node);
void event_queue_mark_uploaded(const char *event_id);
void event_queue_release_payload(bodyguard_event_t *event);
const char *event_queue_current_event_id(void);
void event_queue_set_current_event_id(const char *event_id);

#ifdef __cplusplus
}
#endif
