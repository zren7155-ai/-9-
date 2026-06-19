#pragma once

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVENT_TRIGGER_WARNING = 1,
    EVENT_TRIGGER_CONFIRM,
    EVENT_TRIGGER_EMERGENCY,
} event_trigger_type_t;

typedef struct {
    event_trigger_type_t type;
    risk_sample_t risk;
    uint64_t timestamp_ms;
} event_trigger_t;

esp_err_t event_router_init(void);
esp_err_t event_router_start(void);
bool event_router_post_trigger(const event_trigger_t *trigger);

#ifdef __cplusplus
}
#endif
