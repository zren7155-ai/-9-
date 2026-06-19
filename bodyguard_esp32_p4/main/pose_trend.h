#pragma once

#include "event_queue.h"
#include "learning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void pose_trend_reset(void);
void pose_trend_push(const sensor_sample_t *sample);
pose_trend_result_t pose_trend_get(void);

#ifdef __cplusplus
}
#endif
