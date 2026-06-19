#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_TTS_CODE_ANALYZING      "ANALYZING"
#define AUDIO_TTS_CODE_FALL_DETECTED  "FALL_DETECTED"
#define AUDIO_TTS_CODE_FATIGUE        "FATIGUE"
#define AUDIO_TTS_CODE_NORMAL         "NORMAL"

esp_err_t audio_alert_init(void);
esp_err_t audio_alert_start(void);
void audio_alert_on_risk_event(const char *event_id, uint8_t risk_pre);
void audio_alert_on_warning(uint8_t risk_pre);
void audio_alert_on_emergency_event(const char *event_id);
void audio_alert_on_ai_result(const char *event_id, uint8_t ai_result, const char *tts_code);

#ifdef __cplusplus
}
#endif
