#pragma once

#include "esp_err.h"
#include "event_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t eeg_init(QueueHandle_t output_queue);
esp_err_t eeg_start(void);
void eeg_set_connected(bool connected);
void eeg_ingest_brainlink_payload(const uint8_t *data, uint16_t len);
void eeg_ingest_pc_bridge(uint8_t attention, uint8_t meditation, uint8_t signal);
bool eeg_is_connected(void);
eeg_sample_t eeg_get_latest(void);

#ifdef __cplusplus
}
#endif
