#pragma once

#include "event_queue.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void eeg_trend_reset(void);
void eeg_trend_push(const eeg_sample_t *sample);
eeg_trend_result_t eeg_trend_get(void);

#ifdef __cplusplus
}
#endif
