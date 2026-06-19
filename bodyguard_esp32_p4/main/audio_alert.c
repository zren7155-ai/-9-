#include "audio_alert.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bodyguard_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "event_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "risk_engine.h"
#include "sensor.h"
#include "voice_assets.h"

static const char *TAG = "audio_manager";
static const uint8_t ES8311_ADDR = 0x18;

#define ES8311_RESET_REG00       0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09       0x09
#define ES8311_SDPOUT_REG0A      0x0A
#define ES8311_SYSTEM_REG0D      0x0D
#define ES8311_SYSTEM_REG0E      0x0E
#define ES8311_SYSTEM_REG12      0x12
#define ES8311_SYSTEM_REG13      0x13
#define ES8311_SYSTEM_REG14      0x14
#define ES8311_ADC_REG17         0x17
#define ES8311_ADC_REG1C         0x1C
#define ES8311_DAC_REG32         0x32
#define ES8311_DAC_REG37         0x37

typedef enum {
    VOICE_REQ_ANALYZING = 1,
    VOICE_REQ_AI_FALL,
    VOICE_REQ_AI_FATIGUE,
    VOICE_REQ_EMERGENCY_TONE,
} voice_req_type_t;

typedef struct {
    bool detect_played;
    bool ai_played;
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint64_t last_voice_time;
    uint64_t normal_since_ms;
    bool active;
} voice_state_t;

typedef struct {
    voice_req_type_t type;
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    uint8_t priority;
} voice_request_t;

typedef struct {
    TaskHandle_t task_handle;
    QueueHandle_t queue;
    i2s_chan_handle_t tx_handle;
    i2c_master_dev_handle_t codec_handle;
    voice_state_t voice_state;
    bool initialized;
    bool audio_ready;
    bool emergency_latched;
    bool warning_latched;
    bool self_test_done;
    portMUX_TYPE lock;
} audio_ctx_t;

static audio_ctx_t s_ctx = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

#ifndef BODYGUARD_AUDIO_BOOT_SELF_TEST
#define BODYGUARD_AUDIO_BOOT_SELF_TEST 0
#endif

static esp_err_t codec_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_ctx.codec_handle, payload, sizeof(payload), -1);
}

static bool cooldown_ready_locked(uint64_t now)
{
    return s_ctx.voice_state.last_voice_time == 0 ||
           now - s_ctx.voice_state.last_voice_time >= BODYGUARD_VOICE_COOLDOWN_MS;
}

static bool enqueue_voice(voice_req_type_t type, const char *event_id, uint8_t priority)
{
    if (s_ctx.queue == NULL || event_id == NULL || event_id[0] == '\0') {
        return false;
    }

    voice_request_t req = {
        .type = type,
        .priority = priority,
    };
    strlcpy(req.event_id, event_id, sizeof(req.event_id));

    if (xQueueSend(s_ctx.queue, &req, 0) != pdTRUE) {
        voice_request_t drop;
        (void)xQueueReceive(s_ctx.queue, &drop, 0);
        return xQueueSend(s_ctx.queue, &req, 0) == pdTRUE;
    }
    return true;
}

static void fill_tone(int16_t *buffer, size_t sample_count, uint32_t freq_hz, int16_t amplitude)
{
    static uint32_t phase;
    const float step = 2.0f * 3.14159265f * (float)freq_hz / (float)BODYGUARD_AUDIO_SAMPLE_RATE;

    for (size_t i = 0; i < sample_count; i += 2) {
        int16_t v = (int16_t)(sinf((float)phase * step) * amplitude);
        buffer[i] = v;
        buffer[i + 1] = v;
        phase++;
    }
}

static void play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_ctx.audio_ready) {
        return;
    }

    int16_t pcm[512];
    uint32_t loops = duration_ms / 16;
    if (loops == 0) {
        loops = 1;
    }

    for (uint32_t i = 0; i < loops; i++) {
        size_t written = 0;
        fill_tone(pcm, sizeof(pcm) / sizeof(pcm[0]), freq_hz, 2200);
        (void)i2s_channel_write(s_ctx.tx_handle, pcm, sizeof(pcm), &written, pdMS_TO_TICKS(5));
        esp_task_wdt_reset();
    }
}

static bool wav_read_header(FILE *fp, uint32_t *sample_rate, uint16_t *channels,
                            uint16_t *bits_per_sample, uint32_t *data_size)
{
    uint8_t hdr[44];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return false;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0 ||
        memcmp(hdr + 12, "fmt ", 4) != 0 || memcmp(hdr + 36, "data", 4) != 0) {
        return false;
    }

    uint16_t audio_format = (uint16_t)(hdr[20] | (hdr[21] << 8));
    *channels = (uint16_t)(hdr[22] | (hdr[23] << 8));
    *sample_rate = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                   ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    *bits_per_sample = (uint16_t)(hdr[34] | (hdr[35] << 8));
    *data_size = (uint32_t)hdr[40] | ((uint32_t)hdr[41] << 8) |
                 ((uint32_t)hdr[42] << 16) | ((uint32_t)hdr[43] << 24);
    return audio_format == 1 && *channels == 2 &&
           *sample_rate == BODYGUARD_AUDIO_SAMPLE_RATE &&
           *bits_per_sample == 16;
}

static bool play_wav_file(const char *path)
{
    if (!s_ctx.audio_ready || path == NULL) {
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    uint32_t sample_rate = 0;
    uint32_t data_size = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    if (!wav_read_header(fp, &sample_rate, &channels, &bits, &data_size)) {
        fclose(fp);
        ESP_LOGW(TAG, "unsupported WAV, fallback to tone: %s", path);
        return false;
    }

    uint8_t buf[1024];
    uint32_t remaining = data_size;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        size_t got = fread(buf, 1, chunk, fp);
        if (got == 0) {
            break;
        }
        size_t written = 0;
        (void)i2s_channel_write(s_ctx.tx_handle, buf, got, &written, pdMS_TO_TICKS(20));
        remaining -= got;
        esp_task_wdt_reset();
    }

    fclose(fp);
    return true;
}

static bool play_embedded_pcm_gain(const int16_t *pcm, size_t pcm_len, int gain)
{
    if (!s_ctx.audio_ready || pcm == NULL || pcm_len == 0) {
        return false;
    }

    int16_t out[512];
    size_t offset = 0;
    while (offset < pcm_len) {
        size_t chunk_samples = pcm_len - offset;
        if (chunk_samples > (sizeof(out) / sizeof(out[0]))) {
            chunk_samples = sizeof(out) / sizeof(out[0]);
        }
        for (size_t i = 0; i < chunk_samples; i++) {
            int32_t amplified = (int32_t)pcm[offset + i] * gain;
            if (amplified > 32767) {
                amplified = 32767;
            } else if (amplified < -32768) {
                amplified = -32768;
            }
            out[i] = (int16_t)amplified;
        }

        size_t written = 0;
        (void)i2s_channel_write(s_ctx.tx_handle, out, chunk_samples * sizeof(int16_t),
                                &written, pdMS_TO_TICKS(20));
        if (written == 0) {
            return false;
        }
        offset += written / sizeof(int16_t);
        esp_task_wdt_reset();
    }
    return true;
}

static bool play_embedded_pcm(const int16_t *pcm, size_t pcm_len)
{
    return play_embedded_pcm_gain(pcm, pcm_len, 2);
}

static void play_voice_request(const voice_request_t *req)
{
    if (req == NULL) {
        return;
    }

    switch (req->type) {
    case VOICE_REQ_ANALYZING:
        ESP_LOGI(TAG, "voice analyzing event_id=%s text=正在分析，请注意安全", req->event_id);
        if (!play_embedded_pcm(g_voice_analyzing_pcm, g_voice_analyzing_pcm_len) &&
            !play_wav_file(BODYGUARD_VOICE_WAV_ANALYZING)) {
            play_tone(660, 120);
            vTaskDelay(pdMS_TO_TICKS(40));
            play_tone(880, 120);
        }
        break;
    case VOICE_REQ_AI_FALL:
        ESP_LOGW(TAG, "voice AI fall event_id=%s text=疑似跌倒，请立即查看", req->event_id);
        if (!play_embedded_pcm(g_voice_fall_detected_pcm, g_voice_fall_detected_pcm_len) &&
            !play_wav_file(BODYGUARD_VOICE_WAV_FALL)) {
            play_tone(1200, 220);
            vTaskDelay(pdMS_TO_TICKS(60));
            play_tone(1200, 220);
        }
        break;
    case VOICE_REQ_AI_FATIGUE:
        ESP_LOGW(TAG, "voice AI fatigue event_id=%s text=检测到疲劳，请注意休息", req->event_id);
        if (!play_embedded_pcm(g_voice_fatigue_pcm, g_voice_fatigue_pcm_len) &&
            !play_wav_file(BODYGUARD_VOICE_WAV_FATIGUE)) {
            play_tone(760, 180);
        }
        break;
    case VOICE_REQ_EMERGENCY_TONE:
        play_tone(1200, 120);
        break;
    default:
        break;
    }
}

static void voice_state_reset_locked(void)
{
    memset(&s_ctx.voice_state, 0, sizeof(s_ctx.voice_state));
    s_ctx.emergency_latched = false;
}

static void audio_update_recovery(const risk_sample_t *risk)
{
    uint64_t now = bodyguard_now_ms();
    taskENTER_CRITICAL(&s_ctx.lock);
    if (!s_ctx.voice_state.active) {
        taskEXIT_CRITICAL(&s_ctx.lock);
        return;
    }

    if (risk->risk_pre < BODYGUARD_VOICE_RESET_RISK) {
        if (s_ctx.voice_state.normal_since_ms == 0) {
            s_ctx.voice_state.normal_since_ms = now;
        } else if (now - s_ctx.voice_state.normal_since_ms >= BODYGUARD_VOICE_RESET_HOLD_MS) {
            ESP_LOGI(TAG, "voice state reset after normal hold");
            voice_state_reset_locked();
        }
    } else {
        s_ctx.voice_state.normal_since_ms = 0;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);
}

static esp_err_t audio_power_amp_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BODYGUARD_AUDIO_PA_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "PA_EN GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BODYGUARD_AUDIO_PA_EN_GPIO, 1), TAG, "PA_EN enable failed");
    return ESP_OK;
}

static esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BODYGUARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_ctx.tx_handle, NULL), TAG, "create I2S TX failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BODYGUARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BODYGUARD_AUDIO_I2S_MCLK_GPIO,
            .bclk = BODYGUARD_AUDIO_I2S_BCLK_GPIO,
            .ws = BODYGUARD_AUDIO_I2S_WS_GPIO,
            .dout = BODYGUARD_AUDIO_I2S_DOUT_GPIO,
            .din = BODYGUARD_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = BODYGUARD_AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_ctx.tx_handle, &std_cfg), TAG, "init I2S std failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_ctx.tx_handle), TAG, "enable I2S TX failed");
    return ESP_OK;
}

static esp_err_t audio_codec_init(void)
{
    i2c_master_bus_handle_t bus = sensor_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "audio I2C bus unavailable");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_ctx.codec_handle), TAG, "add ES8311 failed");

    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_RESET_REG00, 0x1F), TAG, "ES8311 reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_RESET_REG00, 0x00), TAG, "ES8311 reset release failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_RESET_REG00, 0x80), TAG, "ES8311 power failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F), TAG, "ES8311 clk1 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG02, 0x20), TAG, "ES8311 clk2 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG03, 0x10), TAG, "ES8311 clk3 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG04, 0x10), TAG, "ES8311 clk4 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG05, 0x00), TAG, "ES8311 clk5 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG06, 0x04), TAG, "ES8311 clk6 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG07, 0x00), TAG, "ES8311 clk7 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF), TAG, "ES8311 clk8 failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SDPIN_REG09, 0x0C), TAG, "ES8311 sdpin failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SDPOUT_REG0A, 0x0C), TAG, "ES8311 sdpout failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SYSTEM_REG0D, 0x01), TAG, "ES8311 analog failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SYSTEM_REG0E, 0x02), TAG, "ES8311 adc power failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SYSTEM_REG12, 0x00), TAG, "ES8311 dac power failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SYSTEM_REG13, 0x10), TAG, "ES8311 output failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_SYSTEM_REG14, 0x1A), TAG, "ES8311 pga failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_ADC_REG17, 0xC8), TAG, "ES8311 adc gain failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_ADC_REG1C, 0x6A), TAG, "ES8311 adc eq failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_DAC_REG37, 0x08), TAG, "ES8311 dac eq failed");
    ESP_RETURN_ON_ERROR(codec_write_reg(ES8311_DAC_REG32, (uint8_t)((BODYGUARD_AUDIO_VOLUME * 256 / 100) - 1)),
                        TAG, "ES8311 volume failed");
    return ESP_OK;
}

static void audio_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "audio_task started");

    while (true) {
        if (!s_ctx.self_test_done) {
            s_ctx.self_test_done = true;
            if (BODYGUARD_AUDIO_BOOT_SELF_TEST && s_ctx.audio_ready) {
                ESP_LOGI(TAG, "audio self test: 正在分析，请注意安全");
                if (!play_embedded_pcm(g_voice_analyzing_pcm, g_voice_analyzing_pcm_len)) {
                    play_tone(880, 120);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    play_tone(1200, 120);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(350));
                    (void)play_embedded_pcm(g_voice_analyzing_pcm, g_voice_analyzing_pcm_len);
                }
            }
        }

        risk_sample_t risk = risk_engine_get_latest();
        audio_update_recovery(&risk);

        if (risk.sys_state == 3) {
            bool should_beep = false;
            taskENTER_CRITICAL(&s_ctx.lock);
            if (!s_ctx.emergency_latched) {
                s_ctx.emergency_latched = true;
                should_beep = true;
            }
            taskEXIT_CRITICAL(&s_ctx.lock);

            if (should_beep) {
                voice_request_t req = {.type = VOICE_REQ_EMERGENCY_TONE};
                strlcpy(req.event_id, event_queue_current_event_id(), sizeof(req.event_id));
                play_voice_request(&req);
            }
        }

        if (risk.sys_state < 2) {
            taskENTER_CRITICAL(&s_ctx.lock);
            s_ctx.warning_latched = false;
            taskEXIT_CRITICAL(&s_ctx.lock);
        }

        voice_request_t req;
        if (xQueueReceive(s_ctx.queue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
            play_voice_request(&req);
        }

        esp_task_wdt_reset();
    }
}

esp_err_t audio_alert_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_ctx.queue = xQueueCreate(6, sizeof(voice_request_t));
    if (s_ctx.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_power_amp_init();
    if (err == ESP_OK) {
        err = audio_i2s_init();
    }
    if (err == ESP_OK) {
        err = audio_codec_init();
    }

    s_ctx.audio_ready = (err == ESP_OK);
    s_ctx.initialized = true;
    if (s_ctx.audio_ready) {
        ESP_LOGI(TAG, "ES8311 + NS4150B audio manager initialized");
    } else {
        ESP_LOGW(TAG, "audio hardware init failed, voice manager keeps log channel: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t audio_alert_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(audio_task, "audio_task", BODYGUARD_TASK_STACK_AUDIO,
                                NULL, 4, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void audio_alert_on_risk_event(const char *event_id, uint8_t risk_pre)
{
    if (!s_ctx.initialized || event_id == NULL || event_id[0] == '\0' ||
        risk_pre < BODYGUARD_RISK_TRIGGER_THRESHOLD) {
        return;
    }

    uint64_t now = bodyguard_now_ms();
    bool should_enqueue = false;

    taskENTER_CRITICAL(&s_ctx.lock);
    if (!s_ctx.voice_state.active ||
        strncmp(s_ctx.voice_state.event_id, event_id, sizeof(s_ctx.voice_state.event_id)) != 0) {
        memset(&s_ctx.voice_state, 0, sizeof(s_ctx.voice_state));
        strlcpy(s_ctx.voice_state.event_id, event_id, sizeof(s_ctx.voice_state.event_id));
        s_ctx.voice_state.active = true;
        s_ctx.emergency_latched = false;
    }

    if (!s_ctx.voice_state.detect_played && cooldown_ready_locked(now)) {
        s_ctx.voice_state.detect_played = true;
        s_ctx.voice_state.last_voice_time = now;
        should_enqueue = true;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (should_enqueue) {
        (void)enqueue_voice(VOICE_REQ_ANALYZING, event_id, 1);
    }
}

void audio_alert_on_warning(uint8_t risk_pre)
{
    if (!s_ctx.initialized || risk_pre < BODYGUARD_RISK_TRIGGER_THRESHOLD) {
        return;
    }

    uint64_t now = bodyguard_now_ms();
    char event_id[BODYGUARD_EVENT_ID_STR_LEN];
    snprintf(event_id, sizeof(event_id), "warn-%llu", (unsigned long long)now);

    bool should_enqueue = false;
    taskENTER_CRITICAL(&s_ctx.lock);
    if (!s_ctx.warning_latched && cooldown_ready_locked(now)) {
        s_ctx.warning_latched = true;
        s_ctx.voice_state.last_voice_time = now;
        should_enqueue = true;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (should_enqueue) {
        (void)enqueue_voice(VOICE_REQ_ANALYZING, event_id, 1);
    }
}

static void audio_alert_force_analyzing(const char *event_id)
{
    if (!s_ctx.initialized || event_id == NULL || event_id[0] == '\0') {
        return;
    }

    uint64_t now = bodyguard_now_ms();
    bool should_enqueue = false;

    taskENTER_CRITICAL(&s_ctx.lock);
    if (!s_ctx.voice_state.active ||
        strncmp(s_ctx.voice_state.event_id, event_id, sizeof(s_ctx.voice_state.event_id)) != 0) {
        memset(&s_ctx.voice_state, 0, sizeof(s_ctx.voice_state));
        strlcpy(s_ctx.voice_state.event_id, event_id, sizeof(s_ctx.voice_state.event_id));
        s_ctx.voice_state.active = true;
        s_ctx.emergency_latched = false;
    }

    if (!s_ctx.voice_state.detect_played) {
        s_ctx.voice_state.detect_played = true;
        s_ctx.voice_state.last_voice_time = now;
        should_enqueue = true;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (should_enqueue) {
        (void)enqueue_voice(VOICE_REQ_ANALYZING, event_id, 3);
    }
}

void audio_alert_on_emergency_event(const char *event_id)
{
    audio_alert_force_analyzing(event_id);
}

void audio_alert_on_ai_result(const char *event_id, uint8_t ai_result, const char *tts_code)
{
    if (!s_ctx.initialized || event_id == NULL || event_id[0] == '\0') {
        return;
    }
    if (ai_result == BODYGUARD_AI_NORMAL ||
        (tts_code != NULL && strcmp(tts_code, AUDIO_TTS_CODE_NORMAL) == 0)) {
        return;
    }

    voice_req_type_t type = 0;
    if (ai_result == BODYGUARD_AI_DANGER ||
        (tts_code != NULL && strcmp(tts_code, AUDIO_TTS_CODE_FALL_DETECTED) == 0)) {
        type = VOICE_REQ_AI_FALL;
    } else if (tts_code != NULL && strcmp(tts_code, AUDIO_TTS_CODE_FATIGUE) == 0) {
        type = VOICE_REQ_AI_FATIGUE;
    } else {
        return;
    }

    uint64_t now = bodyguard_now_ms();
    bool should_enqueue = false;

    taskENTER_CRITICAL(&s_ctx.lock);
    if (!s_ctx.voice_state.active ||
        strncmp(s_ctx.voice_state.event_id, event_id, sizeof(s_ctx.voice_state.event_id)) != 0) {
        taskEXIT_CRITICAL(&s_ctx.lock);
        ESP_LOGW(TAG, "ignore voice request for stale event_id=%s", event_id);
        return;
    }

    if (!s_ctx.voice_state.ai_played && cooldown_ready_locked(now)) {
        s_ctx.voice_state.ai_played = true;
        s_ctx.voice_state.last_voice_time = now;
        should_enqueue = true;
    }
    taskEXIT_CRITICAL(&s_ctx.lock);

    if (should_enqueue) {
        (void)enqueue_voice(type, event_id, 3);
    }
}
