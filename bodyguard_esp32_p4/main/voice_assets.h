#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const int16_t g_voice_analyzing_pcm[];
extern const size_t g_voice_analyzing_pcm_len;
extern const int16_t g_voice_fall_detected_pcm[];
extern const size_t g_voice_fall_detected_pcm_len;
extern const int16_t g_voice_fatigue_pcm[];
extern const size_t g_voice_fatigue_pcm_len;

#ifdef __cplusplus
}
#endif
