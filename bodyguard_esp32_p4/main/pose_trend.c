#include "pose_trend.h"

#include <math.h>
#include <string.h>

#define POSE_TREND_WINDOW 16

typedef struct {
    sensor_sample_t ring[POSE_TREND_WINDOW];
    size_t write;
    size_t count;
    pose_trend_result_t latest;
} pose_trend_ctx_t;

static pose_trend_ctx_t s_ctx;

static uint8_t clamp_score_i32(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    return v > 100 ? 100 : (uint8_t)v;
}

void pose_trend_reset(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void pose_trend_push(const sensor_sample_t *sample)
{
    if (sample == NULL || sample->timestamp_ms == 0) {
        return;
    }

    s_ctx.ring[s_ctx.write] = *sample;
    s_ctx.write = (s_ctx.write + 1) % POSE_TREND_WINDOW;
    if (s_ctx.count < POSE_TREND_WINDOW) {
        s_ctx.count++;
    }

    if (s_ctx.count < 2) {
        return;
    }

    const size_t newest_idx = (s_ctx.write + POSE_TREND_WINDOW - 1) % POSE_TREND_WINDOW;
    const size_t oldest_idx = (s_ctx.write + POSE_TREND_WINDOW - s_ctx.count) % POSE_TREND_WINDOW;
    const sensor_sample_t *oldest = &s_ctx.ring[oldest_idx];
    const sensor_sample_t *newest = &s_ctx.ring[newest_idx];
    uint64_t dt_ms = newest->timestamp_ms > oldest->timestamp_ms ?
                     newest->timestamp_ms - oldest->timestamp_ms : 1;
    float dt_s = (float)dt_ms / 1000.0f;

    float accel_change = 0.0f;
    float gyro_drift = 0.0f;
    for (int i = 0; i < 3; i++) {
        accel_change += fabsf(newest->accel[i] - oldest->accel[i]);
        gyro_drift += fabsf(newest->gyro[i] - oldest->gyro[i]);
    }

    float tilt_delta = newest->tilt_angle - oldest->tilt_angle;
    pose_trend_result_t r = {0};
    r.tilt_speed_deg_per_sec = tilt_delta > 0.0f ? tilt_delta / dt_s : 0.0f;
    r.accel_change = accel_change;
    r.gyro_drift = gyro_drift;
    r.fall_trend = r.tilt_speed_deg_per_sec >= 60.0f && newest->tilt_angle >= 45.0f;
    r.long_imbalance = newest->tilt_angle >= 45.0f && dt_ms >= 1200;
    r.trend_score = clamp_score_i32((int32_t)(r.tilt_speed_deg_per_sec * 0.7f +
                                             r.accel_change * 8.0f +
                                             r.gyro_drift * 0.2f +
                                             (r.long_imbalance ? 20 : 0)));
    s_ctx.latest = r;
}

pose_trend_result_t pose_trend_get(void)
{
    return s_ctx.latest;
}
