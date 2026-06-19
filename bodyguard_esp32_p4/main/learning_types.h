#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bodyguard_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    char danger_type[24];
    char recommendation[24];
    float confidence;
    bool false_alarm;
    float pose_reliable;
    float eeg_reliable;
    float recommended_pose_weight;
    float recommended_eeg_weight;
    bool has_pose_weight;
    bool has_eeg_weight;
} learning_ai_feedback_t;

typedef struct {
    float pose_weight;
    float eeg_weight;
    float burst_weight;
    uint8_t warning_threshold;
    uint8_t danger_threshold;
    float posture_baseline;
    float eeg_baseline;
    float motion_baseline;
    uint32_t false_alarm_count;
    uint32_t confirmed_danger_count;
    uint32_t missed_danger_count;
    float fall_angle_deg;
    uint16_t accel_high_threshold;
    uint16_t gyro_high_threshold;
    uint8_t sensitivity;
    uint32_t update_count;
    uint32_t version;
} adaptive_params_t;

typedef struct {
    float attention_drop_per_sec;
    float fatigue_rise_per_sec;
    uint8_t signal_stability;
    uint8_t trend_score;
    bool consciousness_drop;
    bool signal_unstable;
} eeg_trend_result_t;

typedef struct {
    float tilt_speed_deg_per_sec;
    float gyro_drift;
    float accel_change;
    uint8_t trend_score;
    bool fall_trend;
    bool long_imbalance;
} pose_trend_result_t;

typedef struct {
    eeg_trend_result_t eeg;
    pose_trend_result_t pose;
    uint8_t risk_velocity;
    uint32_t risk_hold_ms;
    bool risk_rising;
    uint64_t timestamp_ms;
} temporal_learning_snapshot_t;

#ifdef __cplusplus
}
#endif
