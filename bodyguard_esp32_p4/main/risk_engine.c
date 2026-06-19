#include "risk_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_alert.h"
#include "camera_ring.h"
#include "adaptive_learning.h"
#include "event_router.h"
#include "learning_loop_engine.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "state_machine.h"

static const char *TAG = "risk_engine";

typedef enum {
    FUSION_STATE_NORMAL = 0,
    FUSION_STATE_WARNING,
    FUSION_STATE_CONFIRM,
    FUSION_STATE_DANGER,
    FUSION_STATE_RECOVERY,
} fusion_state_t;

typedef struct {
    QueueHandle_t sensor_queue;
    QueueHandle_t eeg_queue;
    TaskHandle_t task_handle;
    TaskHandle_t event_task_handle;
    sensor_sample_t last_sensor;
    eeg_sample_t last_eeg;
    risk_sample_t latest_risk;
    sensor_sample_t sensor_ring[BODYGUARD_CAMERA_RING_FRAMES];
    eeg_sample_t eeg_ring[BODYGUARD_CAMERA_RING_FRAMES];
    risk_sample_t risk_ring[BODYGUARD_CAMERA_RING_FRAMES];
    size_t sensor_write;
    size_t eeg_write;
    size_t risk_write;
    uint8_t att_window[3];
    uint8_t att_ptr;
    uint8_t att_base;
    uint64_t eeg_drop_start_ms;
    float last_accel_raw[3];
    float last_gyro_raw[3];
    uint16_t latest_accel_diff;
    uint16_t latest_gyro_diff;
    bool motion_baseline_ready;
    uint8_t burst_count;
    uint8_t fast_trigger_count;
    uint8_t attention_drop_score;
    uint8_t stable_eeg_state;
    uint8_t pending_eeg_state;
    uint64_t eeg_state_start_ms;
    int8_t eeg_modifier;
    uint8_t stable_pose_state;
    uint8_t pending_pose_state;
    uint64_t pose_state_start_ms;
    bool posture_baseline_ready;
    float posture_baseline_accel[3];
    float posture_relative_angle;
    bool candidate_event;
    bool imu_shock_seen;
    bool imu_impact_seen;
    bool imu_rotation_seen;
    bool posture_abnormal_seen;
    bool stillness_seen;
    bool confirm_event_posted;
    bool warning_logged;
    bool fast_trigger_latched;
    uint64_t fast_trigger_latch_until_ms;
    uint64_t candidate_start_ms;
    uint64_t posture_abnormal_start_ms;
    uint64_t stillness_start_ms;
    float angle_ref_500ms;
    uint64_t angle_ref_ms;
    uint64_t normal_posture_start_ms;
    uint64_t recovery_start_ms;
    uint64_t risk_hold_start_ms;
    uint64_t last_trigger_ms;
    char last_event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint8_t ai_result;
    uint8_t last_sys_state;
    bool ai_synced;
    char ai_report[AI_REPORT_LEN];
    fusion_state_t fusion_state;
    bool initialized;
} risk_ctx_t;

static risk_ctx_t s_ctx;

static void post_event_trigger(event_trigger_type_t type, const risk_sample_t *risk);

static const char *DEFAULT_AI_REPORT = "等待AI分析...";
static const char *ANALYZING_AI_REPORT = "正在分析，请注意安全";

#define FUSION_WARNING_THRESHOLD          55
#define FUSION_CONFIRM_THRESHOLD          70
#define FUSION_WARNING_CONFIDENCE         55
#define FUSION_CONFIRM_CONFIDENCE         70
#define FUSION_DANGER_CONFIDENCE          82
#define FUSION_WARNING_HOLD_MS            1500
#define FUSION_CONFIRM_HOLD_MS            2500
#define FUSION_DANGER_HOLD_MS             3500
#define FUSION_FAST_TRIGGER_LATCH_MS      5000
#define FUSION_MIN_EVENT_HOLD_MS          650
#define FUSION_WARNING_POSTURE_HOLD_MS    1800
#define FUSION_STILLNESS_HOLD_MS          1200
#define FUSION_EVENT_TIMEOUT_MS           12000
#define FUSION_NORMAL_RESET_MS            2500
#define FUSION_IMPACT_ACCEL_G             1.85f
#define FUSION_ROTATION_DPS               340.0f
#define FUSION_POSTURE_ABNORMAL_DEG       60.0f
#define FUSION_POSTURE_SOFT_DEG           45.0f
#define FUSION_STILL_ACCEL_MAX_G          1.18f
#define FUSION_STILL_GYRO_MAX_DPS         35.0f
#define FUSION_BASELINE_GYRO_MAX_DPS      8.0f
#define FUSION_BASELINE_ACCEL_MIN_G       0.85f
#define FUSION_BASELINE_ACCEL_MAX_G       1.15f

#ifndef BODYGUARD_VIDEO_DEMO_MODE
#define BODYGUARD_VIDEO_DEMO_MODE         0
#endif

static uint8_t clamp_u8_i32(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    return value > 100 ? 100 : (uint8_t)value;
}

static uint16_t absdiff_scaled(float a, float b, float scale)
{
    float diff = fabsf(a - b) * scale;
    if (diff > 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)diff;
}

static uint8_t raw_pose_state_from_tilt(float tilt)
{
    if (tilt > 70.0f) {
        return 3;
    }
    if (tilt > 45.0f) {
        return 2;
    }
    if (tilt > 10.0f) {
        return 1;
    }
    return 0;
}

static uint8_t compute_pose_score(const sensor_sample_t *sample, uint8_t *pose_state)
{
    uint64_t now = bodyguard_now_ms();
    uint8_t raw_state = raw_pose_state_from_tilt(s_ctx.posture_relative_angle);

    if (raw_state != s_ctx.pending_pose_state) {
        s_ctx.pending_pose_state = raw_state;
        s_ctx.pose_state_start_ms = now;
        *pose_state = s_ctx.stable_pose_state;
    } else if (now - s_ctx.pose_state_start_ms >= BODYGUARD_POSTURE_STABLE_MS) {
        if (raw_state >= 2 &&
            !s_ctx.candidate_event &&
            !s_ctx.imu_shock_seen &&
            s_ctx.latest_accel_diff < BODYGUARD_ACCEL_DIFF_HIGH) {
            s_ctx.stable_pose_state = POSTURE_TILT;
        } else {
            s_ctx.stable_pose_state = raw_state;
        }
        *pose_state = s_ctx.stable_pose_state;
    } else {
        *pose_state = s_ctx.stable_pose_state;
    }

    switch (s_ctx.stable_pose_state) {
    case 3:
        return s_ctx.candidate_event ? 90 : 62;
    case 2:
        return s_ctx.candidate_event ? 62 : 38;
    case 1:
        return 20;
    default:
        return 5;
    }
}

static float vector_abs_sum3(const float v[3])
{
    return fabsf(v[0]) + fabsf(v[1]) + fabsf(v[2]);
}

static float vector_norm3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static float vector_angle_deg3(const float a[3], const float b[3])
{
    float na = vector_norm3(a);
    float nb = vector_norm3(b);
    if (na < 0.001f || nb < 0.001f) {
        return 0.0f;
    }
    float cos_theta = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb);
    if (cos_theta > 1.0f) {
        cos_theta = 1.0f;
    } else if (cos_theta < -1.0f) {
        cos_theta = -1.0f;
    }
    return acosf(cos_theta) * 180.0f / 3.14159265f;
}

static void update_posture_baseline(const sensor_sample_t *sample, float accel_total, float gyro_total)
{
    (void)accel_total;
    (void)gyro_total;
    if (!s_ctx.posture_baseline_ready && sample->timestamp_ms != 0) {
        s_ctx.posture_baseline_ready = true;
        ESP_LOGI(TAG, "posture baseline uses calibrated sensor angle=%.1f", sample->tilt_angle);
    }
    s_ctx.posture_relative_angle = sample->tilt_angle;
}

static uint8_t update_attention_drop(uint8_t attention_now)
{
    uint64_t now = bodyguard_now_ms();

    s_ctx.att_window[s_ctx.att_ptr] = attention_now;
    s_ctx.att_ptr = (s_ctx.att_ptr + 1) % 3;

    uint8_t att_avg = (uint8_t)((s_ctx.att_window[0] +
                                 s_ctx.att_window[1] +
                                 s_ctx.att_window[2]) / 3);
    uint8_t drop = 0;
    if (att_avg >= s_ctx.att_base) {
        s_ctx.att_base = att_avg;
        s_ctx.eeg_drop_start_ms = 0;
        return 0;
    }

    drop = (uint8_t)(s_ctx.att_base - att_avg);
    if (drop < 10) {
        s_ctx.eeg_drop_start_ms = 0;
        return 0;
    }

    if (s_ctx.eeg_drop_start_ms == 0) {
        s_ctx.eeg_drop_start_ms = now;
        return 0;
    }
    if (now - s_ctx.eeg_drop_start_ms < BODYGUARD_EEG_DROP_HOLD_MS) {
        return 0;
    }

    if (drop >= 30) {
        return 35;
    }
    if (drop >= 20) {
        return 20;
    }
    return 10;
}

static uint8_t eeg_state_from_metrics(uint8_t fatigue, uint8_t attention_drop, uint8_t score)
{
    if (fatigue >= 75 || attention_drop >= 35 || score >= 70) {
        return 3;
    }
    if (attention_drop >= 20 || score >= 45) {
        return 2;
    }
    if (fatigue >= 40 || score >= 20) {
        return 1;
    }
    return 0;
}

static uint8_t stabilize_eeg_state(uint8_t raw_state)
{
    uint64_t now = bodyguard_now_ms();
    uint32_t hold_ms = raw_state > s_ctx.stable_eeg_state ? 2000U : 1000U;

    if (raw_state == s_ctx.stable_eeg_state) {
        s_ctx.pending_eeg_state = raw_state;
        s_ctx.eeg_state_start_ms = 0;
        return s_ctx.stable_eeg_state;
    }

    if (raw_state != s_ctx.pending_eeg_state) {
        s_ctx.pending_eeg_state = raw_state;
        s_ctx.eeg_state_start_ms = now;
        return s_ctx.stable_eeg_state;
    }

    if (s_ctx.eeg_state_start_ms != 0 &&
        now - s_ctx.eeg_state_start_ms >= hold_ms) {
        s_ctx.stable_eeg_state = raw_state;
        s_ctx.eeg_state_start_ms = 0;
    }
    return s_ctx.stable_eeg_state;
}

static uint8_t compute_eeg_score(const eeg_sample_t *sample, uint8_t *eeg_state)
{
    if (!sample->connected) {
        s_ctx.stable_eeg_state = 0;
        s_ctx.pending_eeg_state = 0;
        s_ctx.eeg_state_start_ms = 0;
        *eeg_state = 0;
        s_ctx.attention_drop_score = 0;
        s_ctx.eeg_modifier = 0;
        return 0;
    }

    uint16_t score = sample->fatigue;
    uint8_t attention_drop = update_attention_drop(sample->attention);
    int8_t modifier = 0;
    s_ctx.attention_drop_score = attention_drop;
    if (attention_drop >= 35) {
        score += 10;
        modifier += 6;
    } else if (attention_drop >= 20) {
        score += 6;
        modifier += 4;
    } else if (attention_drop >= 10) {
        score += 3;
        modifier += 2;
    }
    if (sample->fatigue >= 75) {
        modifier += 8;
    } else if (sample->fatigue >= 60) {
        modifier += 4;
    } else if (sample->fatigue >= 45) {
        modifier += 2;
    }
    if (sample->signal_quality <= (100 - BODYGUARD_EEG_SIGNAL_BAD_THRESHOLD) && score >= 5) {
        score -= 8;
        modifier -= 5;
    }
    if (attention_drop == 0 && sample->fatigue < 40 &&
        sample->signal_quality > (100 - BODYGUARD_EEG_SIGNAL_BAD_THRESHOLD)) {
        modifier -= 3;
    }

    uint8_t clamped = score > 100 ? 100 : (uint8_t)score;
    *eeg_state = stabilize_eeg_state(eeg_state_from_metrics(sample->fatigue, attention_drop, clamped));
    if (modifier > 10) {
        modifier = 10;
    } else if (modifier < -5) {
        modifier = -5;
    }
    s_ctx.eeg_modifier = modifier;
    return clamped;
}

static uint8_t compute_burst_score(const sensor_sample_t *sample)
{
    uint16_t accel_diff = 0;
    uint16_t gyro_diff = 0;
    uint8_t burst = 0;

    if (!s_ctx.motion_baseline_ready) {
        for (int i = 0; i < 3; i++) {
            s_ctx.last_accel_raw[i] = sample->accel[i];
            s_ctx.last_gyro_raw[i] = sample->gyro[i];
        }
        s_ctx.motion_baseline_ready = true;
        s_ctx.latest_accel_diff = 0;
        s_ctx.latest_gyro_diff = 0;
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        accel_diff += absdiff_scaled(sample->accel[i], s_ctx.last_accel_raw[i], 8192.0f);
        gyro_diff += absdiff_scaled(sample->gyro[i], s_ctx.last_gyro_raw[i], 65.5f);
        s_ctx.last_accel_raw[i] = sample->accel[i];
        s_ctx.last_gyro_raw[i] = sample->gyro[i];
    }
    s_ctx.latest_accel_diff = accel_diff;
    s_ctx.latest_gyro_diff = gyro_diff;

    if (accel_diff >= BODYGUARD_ACCEL_DIFF_HIGH || gyro_diff >= BODYGUARD_GYRO_DIFF_HIGH) {
        burst = 30;
    } else if (accel_diff >= BODYGUARD_ACCEL_DIFF_MID || gyro_diff >= BODYGUARD_GYRO_DIFF_MID) {
        burst = 15;
    } else if (accel_diff >= BODYGUARD_ACCEL_DIFF_SMALL || gyro_diff >= BODYGUARD_GYRO_DIFF_SMALL) {
        burst = 5;
    }

    if (burst == 0) {
        s_ctx.burst_count = 0;
        return 0;
    }

    if (++s_ctx.burst_count < 2) {
        return 0;
    }
    return burst;
}

static bool imu_fast_trigger(const sensor_sample_t *sample, bool *impact, bool *rotation, bool *fast_angle_out)
{
    float accel_total = vector_norm3(sample->accel);
    float gyro_total = vector_abs_sum3(sample->gyro);
    float posture_angle = s_ctx.posture_relative_angle;
    bool fast_angle = false;
    bool impact_now = accel_total > FUSION_IMPACT_ACCEL_G ||
                      s_ctx.latest_accel_diff >= BODYGUARD_ACCEL_DIFF_HIGH;
    bool rotation_now = gyro_total > FUSION_ROTATION_DPS ||
                        s_ctx.latest_gyro_diff >= BODYGUARD_GYRO_DIFF_HIGH;
    bool instant_trigger = false;
    uint64_t now = bodyguard_now_ms();

    if (s_ctx.angle_ref_ms == 0) {
        s_ctx.angle_ref_ms = sample->timestamp_ms;
        s_ctx.angle_ref_500ms = posture_angle;
    } else {
        uint64_t elapsed_ms = sample->timestamp_ms - s_ctx.angle_ref_ms;
        if (elapsed_ms >= 500) {
            float angle_delta = fabsf(posture_angle - s_ctx.angle_ref_500ms);
            float half_second_delta = angle_delta * 500.0f / (float)elapsed_ms;
            fast_angle = posture_angle > FUSION_POSTURE_SOFT_DEG &&
                         half_second_delta > 60.0f;
            s_ctx.angle_ref_ms = sample->timestamp_ms;
            s_ctx.angle_ref_500ms = posture_angle;
        }
    }

    /*
     * Layer 1 is IMU-first, but a single condition is still too noisy:
     * picking something up can create tilt, and a tap can create impact.
     * Start a candidate only when motion contains a real fall-like pair.
     */
    instant_trigger = (impact_now && (rotation_now || fast_angle)) ||
                      (rotation_now && fast_angle);
    if (impact != NULL) {
        *impact = impact_now;
    }
    if (rotation != NULL) {
        *rotation = rotation_now;
    }
    if (fast_angle_out != NULL) {
        *fast_angle_out = fast_angle;
    }
    if (!instant_trigger) {
        s_ctx.fast_trigger_count = 0;
        if (s_ctx.fast_trigger_latched && now > s_ctx.fast_trigger_latch_until_ms) {
            s_ctx.fast_trigger_latched = false;
        }
        return false;
    }
    s_ctx.fast_trigger_count++;
    s_ctx.fast_trigger_latched = true;
    s_ctx.fast_trigger_latch_until_ms = now + FUSION_FAST_TRIGGER_LATCH_MS;
    return true;
}

static bool posture_is_abnormal(const risk_sample_t *risk)
{
    (void)risk;
    return s_ctx.posture_relative_angle >= FUSION_POSTURE_ABNORMAL_DEG;
}

static uint8_t compute_layered_risk_pre(uint8_t pose_score, uint8_t burst_score)
{
    adaptive_params_t params = adaptive_learning_get_params();
    int32_t score = (int32_t)lroundf((float)pose_score * params.pose_weight +
                                     (float)burst_score * params.burst_weight) +
                    s_ctx.eeg_modifier;
    return clamp_u8_i32(score);
}

static void change_fusion_state(fusion_state_t next, const char *reason, risk_sample_t *risk)
{
    if (s_ctx.fusion_state == next) {
        return;
    }

    ESP_LOGI(TAG,
             "STATE CHANGE: %u -> %u reason=%s tilt=%.1f accel_diff=%u gyro_diff=%u risk=%u confidence=%u",
             (unsigned)s_ctx.fusion_state,
             (unsigned)next,
             reason != NULL ? reason : "unknown",
             s_ctx.posture_relative_angle,
             (unsigned)s_ctx.latest_accel_diff,
             (unsigned)s_ctx.latest_gyro_diff,
             risk != NULL ? risk->risk_final : 0,
             risk != NULL ? risk->confidence : 0);
    s_ctx.fusion_state = next;
}

static uint8_t compute_confidence(const risk_sample_t *risk, bool shock, bool posture_abnormal)
{
    int32_t confidence = 8;

    if (s_ctx.candidate_event) {
        confidence += 12;
    }
    if (s_ctx.imu_impact_seen) {
        confidence += 18;
    }
    if (s_ctx.imu_rotation_seen) {
        confidence += 12;
    }
    if (shock && (s_ctx.imu_impact_seen || s_ctx.imu_rotation_seen)) {
        confidence += 8;
    }
    if (posture_abnormal || s_ctx.posture_abnormal_seen) {
        confidence += 18;
    }
    if (s_ctx.stillness_seen) {
        confidence += 18;
    }
    if (risk->burst_score >= 15) {
        confidence += 8;
    }
    if (risk->pose_state >= POSTURE_FALL) {
        confidence += 10;
    } else if (risk->pose_state >= POSTURE_FAST_CHANGE) {
        confidence += 5;
    }
    if (s_ctx.posture_abnormal_start_ms != 0) {
        uint64_t held_ms = bodyguard_now_ms() - s_ctx.posture_abnormal_start_ms;
        if (held_ms >= FUSION_DANGER_HOLD_MS) {
            confidence += 10;
        } else if (held_ms >= FUSION_CONFIRM_HOLD_MS) {
            confidence += 5;
        }
    }
    confidence += s_ctx.eeg_modifier;

    return clamp_u8_i32(confidence);
}

static uint8_t state_to_sys_state(fusion_state_t state)
{
    switch (state) {
    case FUSION_STATE_WARNING:
    case FUSION_STATE_CONFIRM:
        return 2;
    case FUSION_STATE_DANGER:
        return 3;
    case FUSION_STATE_NORMAL:
    case FUSION_STATE_RECOVERY:
    default:
        return 1;
    }
}

static void reset_candidate_event(void)
{
    s_ctx.candidate_event = false;
    s_ctx.imu_shock_seen = false;
    s_ctx.imu_impact_seen = false;
    s_ctx.imu_rotation_seen = false;
    s_ctx.posture_abnormal_seen = false;
    s_ctx.stillness_seen = false;
    s_ctx.confirm_event_posted = false;
    s_ctx.warning_logged = false;
    s_ctx.fast_trigger_count = 0;
    s_ctx.candidate_start_ms = 0;
    s_ctx.posture_abnormal_start_ms = 0;
    s_ctx.stillness_start_ms = 0;
    s_ctx.normal_posture_start_ms = 0;
    s_ctx.recovery_start_ms = 0;
}

static void post_event_trigger(event_trigger_type_t type, const risk_sample_t *risk)
{
    if (risk == NULL) {
        return;
    }

    event_trigger_t trigger = {
        .type = type,
        .risk = *risk,
        .timestamp_ms = bodyguard_now_ms(),
    };
    if (!event_router_post_trigger(&trigger)) {
        ESP_LOGW(TAG, "event trigger queue full type=%u risk=%u", type, risk->risk_final);
    }
}

static void update_decision_state(risk_sample_t *risk)
{
    uint64_t now = bodyguard_now_ms();
    bool impact_now = false;
    bool rotation_now = false;
    bool fast_angle = false;
    bool fast_trigger = imu_fast_trigger(&s_ctx.last_sensor, &impact_now, &rotation_now, &fast_angle);
    bool posture_abnormal = posture_is_abnormal(risk);
    float accel_total = vector_norm3(s_ctx.last_sensor.accel);
    float gyro_total = vector_abs_sum3(s_ctx.last_sensor.gyro);
    bool still_now = accel_total < FUSION_STILL_ACCEL_MAX_G &&
                     gyro_total < FUSION_STILL_GYRO_MAX_DPS;
    bool series_ok = false;
    bool confirm_ready = false;
    bool danger_ready = false;

    if (fast_trigger && !s_ctx.candidate_event) {
        s_ctx.candidate_event = true;
        s_ctx.candidate_start_ms = now;
        s_ctx.imu_shock_seen = impact_now || rotation_now || fast_angle;
        s_ctx.imu_impact_seen = impact_now;
        s_ctx.imu_rotation_seen = rotation_now || fast_angle;
        s_ctx.posture_abnormal_seen = posture_abnormal;
        s_ctx.confirm_event_posted = false;
        s_ctx.warning_logged = false;
        s_ctx.stillness_start_ms = 0;
        s_ctx.stillness_seen = false;
        change_fusion_state(FUSION_STATE_NORMAL, "candidate_start", risk);
        state_machine_set_state(STATE_MONITORING);
        ESP_LOGI(TAG,
                 "candidate_event start source=imu_fast tilt=%.1f accel=%.2f gyro=%.1f accel_diff=%u gyro_diff=%u impact=%u rotation=%u fast_angle=%u",
                 s_ctx.posture_relative_angle,
                 accel_total,
                 gyro_total,
                 (unsigned)s_ctx.latest_accel_diff,
                 (unsigned)s_ctx.latest_gyro_diff,
                 impact_now ? 1 : 0,
                 rotation_now ? 1 : 0,
                 fast_angle ? 1 : 0);
    }

    if (s_ctx.candidate_event) {
        s_ctx.imu_shock_seen = s_ctx.imu_shock_seen || impact_now || rotation_now || fast_angle;
        s_ctx.imu_impact_seen = s_ctx.imu_impact_seen || impact_now;
        s_ctx.imu_rotation_seen = s_ctx.imu_rotation_seen || rotation_now || fast_angle;
        s_ctx.posture_abnormal_seen = s_ctx.posture_abnormal_seen || posture_abnormal;
    }

    if (posture_abnormal) {
        if (s_ctx.posture_abnormal_start_ms == 0) {
            s_ctx.posture_abnormal_start_ms = now;
        }
        series_ok = now - s_ctx.posture_abnormal_start_ms >= 2000;
    } else {
        s_ctx.posture_abnormal_start_ms = 0;
    }

    if (s_ctx.candidate_event && posture_abnormal && still_now) {
        if (s_ctx.stillness_start_ms == 0) {
            s_ctx.stillness_start_ms = now;
        } else if (now - s_ctx.stillness_start_ms >= FUSION_STILLNESS_HOLD_MS) {
            s_ctx.stillness_seen = true;
        }
    } else if (!posture_abnormal || !still_now) {
        s_ctx.stillness_start_ms = 0;
    }

    risk->confidence = compute_confidence(risk, s_ctx.imu_shock_seen, posture_abnormal);
    bool fall_motion_pair = s_ctx.imu_impact_seen &&
                            (s_ctx.imu_rotation_seen || risk->burst_score >= 15);
    bool warning_ready = s_ctx.candidate_event &&
                         fall_motion_pair &&
                         posture_abnormal &&
                         s_ctx.posture_abnormal_seen &&
                         now - s_ctx.candidate_start_ms >= FUSION_WARNING_HOLD_MS &&
                         s_ctx.posture_abnormal_start_ms != 0 &&
                         now - s_ctx.posture_abnormal_start_ms >= FUSION_WARNING_POSTURE_HOLD_MS &&
                         risk->confidence >= FUSION_WARNING_CONFIDENCE;
    confirm_ready = warning_ready &&
                    s_ctx.stillness_seen &&
                    series_ok &&
                    now - s_ctx.candidate_start_ms >= FUSION_CONFIRM_HOLD_MS &&
                    risk->confidence >= FUSION_CONFIRM_CONFIDENCE;
    danger_ready = confirm_ready &&
                   fall_motion_pair &&
                   s_ctx.stillness_seen &&
                   series_ok &&
                   now - s_ctx.candidate_start_ms >= FUSION_DANGER_HOLD_MS &&
                   risk->confidence >= FUSION_DANGER_CONFIDENCE;

    if (s_ctx.candidate_event) {
        static uint64_t last_decision_log_ms;
        if (now - last_decision_log_ms >= 500) {
            ESP_LOGI(TAG,
                     "risk decision candidate=1 pair=%u impact=%u rotation=%u posture_now=%u posture_seen=%u still=%u series_ok=%u "
                     "tilt=%.1f accel=%.2f gyro=%.1f pose=%u burst=%u conf=%u risk=%u held=%llu",
                     fall_motion_pair ? 1 : 0,
                     s_ctx.imu_impact_seen ? 1 : 0,
                     s_ctx.imu_rotation_seen ? 1 : 0,
                     posture_abnormal ? 1 : 0,
                     s_ctx.posture_abnormal_seen ? 1 : 0,
                     s_ctx.stillness_seen ? 1 : 0,
                     series_ok ? 1 : 0,
                     s_ctx.posture_relative_angle,
                     accel_total,
                     gyro_total,
                     risk->pose_state,
                     risk->burst_score,
                     risk->confidence,
                     risk->risk_final,
                     (unsigned long long)(now - s_ctx.candidate_start_ms));
            last_decision_log_ms = now;
        }
    }

    if (s_ctx.candidate_event &&
        now - s_ctx.candidate_start_ms >= FUSION_EVENT_TIMEOUT_MS &&
        !confirm_ready) {
        ESP_LOGI(TAG, "candidate_event timeout reset tilt=%.1f conf=%u", s_ctx.posture_relative_angle, risk->confidence);
        reset_candidate_event();
        change_fusion_state(FUSION_STATE_NORMAL, "candidate_timeout", risk);
    }

    if (danger_ready) {
        change_fusion_state(FUSION_STATE_DANGER, "fall_confirmed", risk);
        if (risk->risk_final < BODYGUARD_EMERGENCY_THRESHOLD) {
            risk->risk_final = BODYGUARD_EMERGENCY_THRESHOLD;
        }
        risk->sys_state = 3;
        state_machine_set_state(STATE_ALERT);
        s_ctx.normal_posture_start_ms = 0;
        s_ctx.recovery_start_ms = 0;
        if (s_ctx.last_sys_state != 3) {
            post_event_trigger(EVENT_TRIGGER_EMERGENCY, risk);
        }
        s_ctx.last_sys_state = risk->sys_state;
        return;
    }

    if (confirm_ready) {
        change_fusion_state(FUSION_STATE_CONFIRM, "impact_tilt_still_confirm", risk);
        if (risk->risk_final < FUSION_CONFIRM_THRESHOLD) {
            risk->risk_final = FUSION_CONFIRM_THRESHOLD;
        }
        risk->sys_state = 2;
        if (!s_ctx.confirm_event_posted) {
            s_ctx.confirm_event_posted = true;
        }
    } else if (warning_ready) {
        change_fusion_state(FUSION_STATE_WARNING, "imu_candidate_series", risk);
        if (risk->risk_final < FUSION_WARNING_THRESHOLD) {
            risk->risk_final = FUSION_WARNING_THRESHOLD;
        }
        risk->sys_state = 2;
        if (!s_ctx.warning_logged) {
            post_event_trigger(EVENT_TRIGGER_WARNING, risk);
            s_ctx.warning_logged = true;
        }
    } else {
        if (s_ctx.fusion_state != FUSION_STATE_RECOVERY) {
            change_fusion_state(FUSION_STATE_NORMAL, "no_confirmed_event", risk);
        }
        risk->sys_state = 1;
    }

    if (s_ctx.candidate_event && !posture_abnormal && accel_total < 1.3f) {
        if (s_ctx.normal_posture_start_ms == 0) {
            s_ctx.normal_posture_start_ms = now;
        }
        if (now - s_ctx.normal_posture_start_ms >= FUSION_NORMAL_RESET_MS && s_ctx.recovery_start_ms == 0) {
            s_ctx.recovery_start_ms = now;
            change_fusion_state(FUSION_STATE_RECOVERY, "posture_recovered", risk);
        } else if (s_ctx.recovery_start_ms != 0 && now - s_ctx.recovery_start_ms >= FUSION_NORMAL_RESET_MS) {
            reset_candidate_event();
            change_fusion_state(FUSION_STATE_NORMAL, "recovery_complete", risk);
            state_machine_set_state(STATE_MONITORING);
            risk->sys_state = 1;
        }
    } else {
        s_ctx.normal_posture_start_ms = 0;
        s_ctx.recovery_start_ms = 0;
    }

    if (!s_ctx.candidate_event) {
        s_ctx.fast_trigger_count = 0;
    }

    risk->sys_state = state_to_sys_state(s_ctx.fusion_state);
    s_ctx.last_sys_state = risk->sys_state;
}

static uint8_t apply_ai_fusion(uint8_t risk_pre, uint8_t ai_result, bool ai_synced)
{
    if (!ai_synced) {
        return risk_pre;
    }

    switch (ai_result) {
    case BODYGUARD_AI_DANGER:
        return clamp_u8_i32((int32_t)risk_pre + 15);
    case BODYGUARD_AI_NORMAL:
        return clamp_u8_i32((int32_t)risk_pre - 20);
    case BODYGUARD_AI_MONITOR:
        return clamp_u8_i32((int32_t)risk_pre + 10);
    default:
        return risk_pre;
    }
}

static void ring_push_sensor(const sensor_sample_t *sample)
{
    s_ctx.sensor_ring[s_ctx.sensor_write] = *sample;
    s_ctx.sensor_write = (s_ctx.sensor_write + 1) % BODYGUARD_CAMERA_RING_FRAMES;
}

static void ring_push_eeg(const eeg_sample_t *sample)
{
    s_ctx.eeg_ring[s_ctx.eeg_write] = *sample;
    s_ctx.eeg_write = (s_ctx.eeg_write + 1) % BODYGUARD_CAMERA_RING_FRAMES;
}

static void ring_push_risk(const risk_sample_t *sample)
{
    s_ctx.risk_ring[s_ctx.risk_write] = *sample;
    s_ctx.risk_write = (s_ctx.risk_write + 1) % BODYGUARD_CAMERA_RING_FRAMES;
}

void risk_engine_export_event_series(event_window_t *window)
{
    if (window == NULL) {
        return;
    }
    for (size_t n = 0; n < BODYGUARD_CAMERA_RING_FRAMES; n++) {
        size_t idx = (s_ctx.sensor_write + n) % BODYGUARD_CAMERA_RING_FRAMES;
        if (s_ctx.sensor_ring[idx].timestamp_ms != 0) {
            window->sensor[window->sensor_count++] = s_ctx.sensor_ring[idx];
        }
    }
    for (size_t n = 0; n < BODYGUARD_CAMERA_RING_FRAMES; n++) {
        size_t idx = (s_ctx.eeg_write + n) % BODYGUARD_CAMERA_RING_FRAMES;
        if (s_ctx.eeg_ring[idx].timestamp_ms != 0) {
            window->eeg[window->eeg_count++] = s_ctx.eeg_ring[idx];
        }
    }
    for (size_t n = 0; n < BODYGUARD_CAMERA_RING_FRAMES; n++) {
        size_t idx = (s_ctx.risk_write + n) % BODYGUARD_CAMERA_RING_FRAMES;
        if (s_ctx.risk_ring[idx].timestamp_ms != 0) {
            window->risk[window->risk_count++] = s_ctx.risk_ring[idx];
        }
    }
}

static void risk_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "risk_calc_task started period=%dms", BODYGUARD_RISK_EVAL_MS);

    while (true) {
        sensor_sample_t sensor;
        eeg_sample_t eeg;

        while (xQueueReceive(s_ctx.sensor_queue, &sensor, 0) == pdTRUE) {
            s_ctx.last_sensor = sensor;
            ring_push_sensor(&sensor);
        }
        while (xQueueReceive(s_ctx.eeg_queue, &eeg, 0) == pdTRUE) {
            s_ctx.last_eeg = eeg;
            ring_push_eeg(&eeg);
        }

        risk_sample_t risk = {0};
        float accel_total = vector_norm3(s_ctx.last_sensor.accel);
        float gyro_total = vector_abs_sum3(s_ctx.last_sensor.gyro);
        update_posture_baseline(&s_ctx.last_sensor, accel_total, gyro_total);
        risk.burst_score = compute_burst_score(&s_ctx.last_sensor);
        risk.pose_score = compute_pose_score(&s_ctx.last_sensor, &risk.pose_state);
        risk.eeg_score = compute_eeg_score(&s_ctx.last_eeg, &risk.eeg_state);
        if (!s_ctx.last_eeg.connected) {
            ESP_LOGW(TAG, "EEG not connected: connected=0 attention=%u fatigue=%u signal=%u eeg_state=%u",
                     s_ctx.last_eeg.attention,
                     s_ctx.last_eeg.fatigue,
                     s_ctx.last_eeg.signal_quality,
                     risk.eeg_state);
        }
        risk.risk_pre = compute_layered_risk_pre(risk.pose_score, risk.burst_score);
        risk.ai_result = s_ctx.ai_result;
        risk.ai_synced = s_ctx.ai_synced;
        strlcpy(risk.ai_report, s_ctx.ai_report[0] ? s_ctx.ai_report : DEFAULT_AI_REPORT, sizeof(risk.ai_report));
        risk.risk_final = apply_ai_fusion(risk.risk_pre, risk.ai_result, risk.ai_synced);
        risk.sys_state = state_to_sys_state(s_ctx.fusion_state);
        update_decision_state(&risk);
        risk.posture = (posture_state_t)risk.pose_state;
        for (int i = 0; i < 3; i++) {
            risk.gyro[i] = (int16_t)(s_ctx.last_sensor.gyro[i] * 100.0f);
        }
        risk.timestamp_ms = bodyguard_now_ms();

        s_ctx.latest_risk = risk;
        ring_push_risk(&risk);
        learning_loop_engine_record_risk(&risk);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_RISK_EVAL_MS));
    }
}

esp_err_t risk_engine_init(QueueHandle_t sensor_queue, QueueHandle_t eeg_queue)
{
    ESP_RETURN_ON_FALSE(sensor_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "sensor queue is null");
    ESP_RETURN_ON_FALSE(eeg_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "eeg queue is null");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.sensor_queue = sensor_queue;
    s_ctx.eeg_queue = eeg_queue;
    s_ctx.last_eeg.attention = 50;
    s_ctx.last_eeg.relax = 50;
    s_ctx.last_eeg.signal_quality = 0;
    s_ctx.att_window[0] = 50;
    s_ctx.att_window[1] = 50;
    s_ctx.att_window[2] = 50;
    s_ctx.att_base = 50;
    strlcpy(s_ctx.ai_report, DEFAULT_AI_REPORT, sizeof(s_ctx.ai_report));
    s_ctx.initialized = true;

    ESP_LOGI(TAG, "risk engine initialized");
    return ESP_OK;
}

esp_err_t risk_engine_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "risk not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(risk_task, "risk_calc_task", BODYGUARD_TASK_STACK_HUGE, NULL,
                                BODYGUARD_TASK_PRIO_RISK, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t risk_engine_event_start(void)
{
    return ESP_OK;
}

risk_sample_t risk_engine_get_latest(void)
{
    return s_ctx.latest_risk;
}

void risk_engine_bind_event_id(const char *event_id)
{
    if (event_id == NULL || event_id[0] == '\0') {
        return;
    }

    strlcpy(s_ctx.last_event_id, event_id, sizeof(s_ctx.last_event_id));
    event_queue_set_current_event_id(event_id);
    s_ctx.ai_result = 0;
    s_ctx.ai_synced = false;
    strlcpy(s_ctx.ai_report, ANALYZING_AI_REPORT, sizeof(s_ctx.ai_report));
    strlcpy(s_ctx.latest_risk.ai_report, s_ctx.ai_report, sizeof(s_ctx.latest_risk.ai_report));
}

void risk_engine_demo_set_latest(const risk_sample_t *risk)
{
    if (risk == NULL) {
        return;
    }
    s_ctx.latest_risk = *risk;
    ring_push_risk(risk);
}

void risk_engine_apply_ai_result(const char *event_id, uint8_t ai_result)
{
    risk_engine_apply_ai_report(event_id, ai_result, NULL);
}

void risk_engine_apply_ai_report(const char *event_id, uint8_t ai_result, const char *report)
{
    if (event_id == NULL || event_id[0] == '\0') {
        return;
    }
    if (strncmp(event_id, s_ctx.last_event_id, sizeof(s_ctx.last_event_id)) != 0) {
        ESP_LOGW(TAG, "ignore AI result for stale event_id=%s", event_id);
        return;
    }
    if (ai_result != BODYGUARD_AI_NORMAL &&
        ai_result != BODYGUARD_AI_MONITOR &&
        ai_result != BODYGUARD_AI_DANGER) {
        ESP_LOGW(TAG, "ignore invalid AI result=%u event_id=%s", ai_result, event_id);
        return;
    }

    s_ctx.ai_result = ai_result;
    s_ctx.ai_synced = true;
    if (report != NULL && report[0] != '\0') {
        strlcpy(s_ctx.ai_report, report, sizeof(s_ctx.ai_report));
    } else if (ai_result == BODYGUARD_AI_DANGER) {
        strlcpy(s_ctx.ai_report, "综合判断:危险;处理建议:立即处理", sizeof(s_ctx.ai_report));
    } else if (ai_result == BODYGUARD_AI_MONITOR) {
        strlcpy(s_ctx.ai_report, "综合判断:关注;处理建议:及时查看", sizeof(s_ctx.ai_report));
    } else {
        strlcpy(s_ctx.ai_report, "综合判断:正常;处理建议:继续监护", sizeof(s_ctx.ai_report));
    }
    s_ctx.latest_risk.ai_result = ai_result;
    s_ctx.latest_risk.ai_synced = true;
    strlcpy(s_ctx.latest_risk.ai_report,
            s_ctx.ai_report[0] ? s_ctx.ai_report : DEFAULT_AI_REPORT,
            sizeof(s_ctx.latest_risk.ai_report));
    s_ctx.latest_risk.risk_final = apply_ai_fusion(s_ctx.latest_risk.risk_pre, ai_result, true);
    s_ctx.latest_risk.sys_state = state_to_sys_state(s_ctx.fusion_state);
    s_ctx.last_sys_state = s_ctx.latest_risk.sys_state;
    learning_loop_engine_record_ai(event_id, ai_result);
    const char *tts_code = AUDIO_TTS_CODE_NORMAL;
    if (ai_result == BODYGUARD_AI_DANGER) {
        tts_code = AUDIO_TTS_CODE_FALL_DETECTED;
    } else if (ai_result == BODYGUARD_AI_MONITOR && s_ctx.latest_risk.eeg_state != 0) {
        tts_code = AUDIO_TTS_CODE_FATIGUE;
    }
    audio_alert_on_ai_result(event_id, ai_result, tts_code);
    ESP_LOGI(TAG, "AI result applied event_id=%s ai=%u risk_final=%u",
             event_id, ai_result, s_ctx.latest_risk.risk_final);
}
