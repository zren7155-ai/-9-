#include "adaptive_learning.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "learning_storage.h"

static const char *TAG = "adaptive_learning";

static adaptive_params_t s_params;
static bool s_initialized;

static float clamp_f(float v, float min_v, float max_v)
{
    if (v < min_v) {
        return min_v;
    }
    return v > max_v ? max_v : v;
}

static uint8_t clamp_u8_i32(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    return v > 100 ? 100 : (uint8_t)v;
}

static void load_default_params(adaptive_params_t *params)
{
    memset(params, 0, sizeof(*params));
    params->pose_weight = 0.70f;
    params->eeg_weight = 0.10f;
    params->burst_weight = 0.20f;
    params->warning_threshold = BODYGUARD_RISK_TRIGGER_THRESHOLD;
    params->danger_threshold = BODYGUARD_EMERGENCY_THRESHOLD;
    params->posture_baseline = 0.0f;
    params->eeg_baseline = 50.0f;
    params->motion_baseline = 0.0f;
    params->fall_angle_deg = BODYGUARD_FALL_ANGLE_DEG;
    params->accel_high_threshold = BODYGUARD_ACCEL_DIFF_HIGH;
    params->gyro_high_threshold = BODYGUARD_GYRO_DIFF_HIGH;
    params->sensitivity = 50;
    params->version = 1;
}

static void normalize_weights(adaptive_params_t *params)
{
    params->pose_weight = clamp_f(params->pose_weight, 0.60f, 0.80f);
    params->eeg_weight = clamp_f(params->eeg_weight, 0.05f, 0.20f);
    params->burst_weight = clamp_f(1.0f - params->pose_weight - params->eeg_weight, 0.05f, 0.25f);

    float sum = params->pose_weight + params->eeg_weight + params->burst_weight;
    if (sum < 0.01f) {
        load_default_params(params);
        return;
    }
    params->pose_weight /= sum;
    params->eeg_weight /= sum;
    params->burst_weight /= sum;
    params->warning_threshold = clamp_u8_i32(params->warning_threshold);
    params->danger_threshold = clamp_u8_i32(params->danger_threshold);
    if (params->warning_threshold < 60) {
        params->warning_threshold = 60;
    }
    if (params->warning_threshold > 80) {
        params->warning_threshold = 80;
    }
    if (params->danger_threshold < 85) {
        params->danger_threshold = 85;
    }
    if (params->danger_threshold > 95) {
        params->danger_threshold = 95;
    }
    if (params->danger_threshold <= params->warning_threshold) {
        params->danger_threshold = params->warning_threshold + 10;
    }
}

esp_err_t adaptive_learning_init(void)
{
    load_default_params(&s_params);

    adaptive_params_t stored = {0};
    esp_err_t err = learning_storage_load_params(&stored);
    if (err == ESP_OK && stored.version == 1) {
        s_params = stored;
        normalize_weights(&s_params);
        ESP_LOGI(TAG, "loaded adaptive params pose=%.2f eeg=%.2f burst=%.2f updates=%lu",
                 s_params.pose_weight, s_params.eeg_weight, s_params.burst_weight,
                 (unsigned long)s_params.update_count);
    } else {
        ESP_LOGI(TAG, "use default adaptive params");
        (void)learning_storage_save_params(&s_params);
    }

    s_initialized = true;
    return ESP_OK;
}

adaptive_params_t adaptive_learning_get_params(void)
{
    if (!s_initialized) {
        adaptive_params_t defaults;
        load_default_params(&defaults);
        return defaults;
    }
    return s_params;
}

uint8_t adaptive_learning_score(uint8_t pose_score, uint8_t eeg_score, uint8_t burst_score)
{
    adaptive_params_t p = adaptive_learning_get_params();
    float score = pose_score * p.pose_weight + eeg_score * p.eeg_weight + burst_score * p.burst_weight;
    return clamp_u8_i32((int32_t)lroundf(score));
}

esp_err_t adaptive_learning_apply_feedback(const learning_ai_feedback_t *feedback)
{
    if (feedback == NULL || feedback->event_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(adaptive_learning_init());
    }

    adaptive_params_t next = s_params;
    const float alpha = 0.15f;

    /* 云端只给建议，ESP32 只允许小步调整规则参数，禁止本地训练模型。 */
    if (feedback->has_pose_weight) {
        next.pose_weight = next.pose_weight * (1.0f - alpha) +
                           clamp_f(feedback->recommended_pose_weight, 0.60f, 0.80f) * alpha;
    }
    if (feedback->has_eeg_weight) {
        next.eeg_weight = next.eeg_weight * (1.0f - alpha) +
                          clamp_f(feedback->recommended_eeg_weight, 0.05f, 0.20f) * alpha;
    }

    if (feedback->false_alarm) {
        /* 误报时略微降低灵敏度，提高强冲击门槛，减少弯腰/走路晃动误报。 */
        next.sensitivity = next.sensitivity > 5 ? (uint8_t)(next.sensitivity - 5) : 0;
        next.accel_high_threshold = (uint16_t)clamp_f(next.accel_high_threshold + 20, 600, 1200);
        next.gyro_high_threshold = (uint16_t)clamp_f(next.gyro_high_threshold + 10, 300, 700);
    } else if (feedback->confidence >= 0.85f) {
        next.sensitivity = next.sensitivity < 95 ? (uint8_t)(next.sensitivity + 2) : 100;
    }

    normalize_weights(&next);
    next.update_count++;
    next.version = 1;
    s_params = next;

    ESP_LOGI(TAG, "adaptive updated event_id=%s pose=%.2f eeg=%.2f burst=%.2f false_alarm=%d",
             feedback->event_id, s_params.pose_weight, s_params.eeg_weight,
             s_params.burst_weight, feedback->false_alarm ? 1 : 0);
    (void)learning_storage_cache_ai_feedback(feedback);
    return learning_storage_save_params(&s_params);
}

esp_err_t adaptive_learning_apply_closed_loop(uint8_t risk_prediction,
                                              uint8_t ai_result,
                                              uint8_t pose_score,
                                              uint8_t eeg_score,
                                              uint8_t burst_score,
                                              const char *event_id)
{
    if (!s_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(adaptive_learning_init());
    }

    adaptive_params_t next = s_params;
    const float step = 0.02f;
    bool changed = false;

    if (risk_prediction >= next.warning_threshold && ai_result == BODYGUARD_AI_NORMAL) {
        next.pose_weight -= pose_score >= eeg_score ? step : 0.0f;
        next.burst_weight -= burst_score > 0 ? step : 0.0f;
        next.eeg_weight += step;
        if (next.warning_threshold < 80) {
            next.warning_threshold += 1;
        }
        next.false_alarm_count++;
        changed = true;
    } else if (risk_prediction < next.warning_threshold && ai_result == BODYGUARD_AI_DANGER) {
        next.pose_weight += pose_score >= eeg_score ? step : 0.0f;
        next.eeg_weight += eeg_score > 20 ? step : 0.0f;
        next.burst_weight += burst_score > 0 ? step : 0.0f;
        if (next.danger_threshold > 85) {
            next.danger_threshold -= 1;
        }
        next.missed_danger_count++;
        changed = true;
    } else if (ai_result == BODYGUARD_AI_DANGER) {
        next.confirmed_danger_count++;
        changed = true;
    }

    if (!changed) {
        return ESP_OK;
    }

    normalize_weights(&next);
    next.update_count++;
    next.version = 1;
    s_params = next;

    ESP_LOGI(TAG, "closed loop updated event=%s ai=%u risk=%u pose=%.2f eeg=%.2f burst=%.2f warn=%u danger=%u",
             event_id != NULL ? event_id : "",
             ai_result, risk_prediction,
             s_params.pose_weight, s_params.eeg_weight, s_params.burst_weight,
             s_params.warning_threshold, s_params.danger_threshold);
    return learning_storage_save_params(&s_params);
}
