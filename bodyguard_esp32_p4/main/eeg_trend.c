#include "eeg_trend.h"

#include <string.h>

#define EEG_TREND_WINDOW 16

typedef struct {
    eeg_sample_t ring[EEG_TREND_WINDOW];
    size_t write;
    size_t count;
    eeg_trend_result_t latest;
} eeg_trend_ctx_t;

static eeg_trend_ctx_t s_ctx;

static uint8_t clamp_score_i32(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    return v > 100 ? 100 : (uint8_t)v;
}

void eeg_trend_reset(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void eeg_trend_push(const eeg_sample_t *sample)
{
    if (sample == NULL || sample->timestamp_ms == 0) {
        return;
    }

    s_ctx.ring[s_ctx.write] = *sample;
    s_ctx.write = (s_ctx.write + 1) % EEG_TREND_WINDOW;
    if (s_ctx.count < EEG_TREND_WINDOW) {
        s_ctx.count++;
    }

    if (s_ctx.count < 2) {
        return;
    }

    const size_t newest_idx = (s_ctx.write + EEG_TREND_WINDOW - 1) % EEG_TREND_WINDOW;
    const size_t oldest_idx = (s_ctx.write + EEG_TREND_WINDOW - s_ctx.count) % EEG_TREND_WINDOW;
    const eeg_sample_t *oldest = &s_ctx.ring[oldest_idx];
    const eeg_sample_t *newest = &s_ctx.ring[newest_idx];
    uint64_t dt_ms = newest->timestamp_ms > oldest->timestamp_ms ?
                     newest->timestamp_ms - oldest->timestamp_ms : 1;
    float dt_s = (float)dt_ms / 1000.0f;

    int att_delta = (int)oldest->attention - (int)newest->attention;
    int fatigue_delta = (int)newest->fatigue - (int)oldest->fatigue;
    uint32_t bad_signal = 0;
    uint32_t signal_sum = 0;
    for (size_t i = 0; i < s_ctx.count; i++) {
        signal_sum += s_ctx.ring[i].signal_quality;
        if (s_ctx.ring[i].signal_quality >= BODYGUARD_EEG_SIGNAL_BAD_THRESHOLD ||
            !s_ctx.ring[i].connected) {
            bad_signal++;
        }
    }

    eeg_trend_result_t r = {0};
    r.attention_drop_per_sec = att_delta > 0 ? (float)att_delta / dt_s : 0.0f;
    r.fatigue_rise_per_sec = fatigue_delta > 0 ? (float)fatigue_delta / dt_s : 0.0f;
    r.signal_stability = clamp_score_i32(100 - (int32_t)(bad_signal * 100 / s_ctx.count));
    r.signal_unstable = r.signal_stability < 60;
    r.consciousness_drop = r.attention_drop_per_sec >= 6.0f && r.fatigue_rise_per_sec >= 1.0f;
    r.trend_score = clamp_score_i32((int32_t)(r.attention_drop_per_sec * 6.0f +
                                             r.fatigue_rise_per_sec * 4.0f +
                                             (r.signal_unstable ? 15 : 0)));
    s_ctx.latest = r;
}

eeg_trend_result_t eeg_trend_get(void)
{
    return s_ctx.latest;
}
