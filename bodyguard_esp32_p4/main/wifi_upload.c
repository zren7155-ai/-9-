#include "wifi_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "event_queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "risk_engine.h"
#include "temporal_learning.h"
#include "esp_wifi.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi_upload";

typedef struct {
    TaskHandle_t task_handle;
    EventGroupHandle_t event_group;
    uint32_t ip4_addr;
    bool initialized;
} wifi_ctx_t;

static wifi_ctx_t s_ctx;

static void upload_delay_ms(uint32_t delay_ms);

static const char *AI_REPORT_PROMPT =
    "Return Chinese only, <=30 Chinese chars. Format: 综合判断 + 处理建议. "
    "Include one risk word: 正常/关注/危险. Example: 综合判断:关注倾斜;处理建议:及时查看";

static size_t utf8_char_len(unsigned char c)
{
    if ((c & 0x80) == 0) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void copy_utf8_chars(char *dst, size_t dst_size, const char *src, size_t max_chars)
{
    size_t out = 0;
    size_t chars = 0;
    if (dst_size == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return;
    }

    while (src[out] != '\0' && chars < max_chars) {
        size_t len = utf8_char_len((unsigned char)src[out]);
        if (out + len >= dst_size) {
            break;
        }
        memcpy(dst + out, src + out, len);
        out += len;
        chars++;
    }
    dst[out] = '\0';
}

static uint8_t parse_ai_result_text(const char *body)
{
    if (body == NULL) {
        return 0;
    }
    if (strstr(body, "AI_DANGER") || strstr(body, "\"ai_result\":3") ||
        strstr(body, "\"ai_result\":\"3\"") || strstr(body, "danger") ||
        strstr(body, "危险")) {
        return BODYGUARD_AI_DANGER;
    }
    if (strstr(body, "AI_MONITOR") || strstr(body, "\"ai_result\":2") ||
        strstr(body, "\"ai_result\":\"2\"") || strstr(body, "monitor") ||
        strstr(body, "关注")) {
        return BODYGUARD_AI_MONITOR;
    }
    if (strstr(body, "AI_NORMAL") || strstr(body, "\"ai_result\":1") ||
        strstr(body, "\"ai_result\":\"1\"") || strstr(body, "normal") ||
        strstr(body, "正常")) {
        return BODYGUARD_AI_NORMAL;
    }
    return 0;
}

static uint8_t parse_ai_result_json(const char *body, char *report_out, size_t report_len)
{
    if (report_out != NULL && report_len > 0) {
        report_out[0] = '\0';
    }
    if (body == NULL) {
        return 0;
    }

    const char *json_start = strchr(body, '{');
    if (json_start == NULL) {
        return parse_ai_result_text(body);
    }

    cJSON *root = cJSON_Parse(json_start);
    if (root == NULL) {
        return parse_ai_result_text(body);
    }

    uint8_t ai = 0;
    cJSON *ai_result = cJSON_GetObjectItemCaseSensitive(root, "ai_result");
    if (cJSON_IsNumber(ai_result)) {
        ai = (uint8_t)ai_result->valueint;
    } else if (cJSON_IsString(ai_result) && ai_result->valuestring != NULL) {
        ai = parse_ai_result_text(ai_result->valuestring);
    }

    if (ai == 0) {
        cJSON *level = cJSON_GetObjectItemCaseSensitive(root, "level");
        if (cJSON_IsString(level) && level->valuestring != NULL) {
            ai = parse_ai_result_text(level->valuestring);
        }
    }

    if (report_out != NULL && report_len > 0) {
        cJSON *report = cJSON_GetObjectItemCaseSensitive(root, "report");
        if (cJSON_IsString(report) && report->valuestring != NULL) {
            strlcpy(report_out, report->valuestring, report_len);
        }
    }

    cJSON_Delete(root);
    if (ai == 0) {
        ai = parse_ai_result_text(body);
    }
    return ai;
}

static void ai_report_payload(const char *body, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (body == NULL) {
        return;
    }

    const char *sep = strchr(body, '|');
    if (sep != NULL && sep[1] != '\0') {
        strlcpy(out, sep + 1, out_len);
        return;
    }

    const char *json_start = strchr(body, '{');
    if (json_start != NULL) {
        cJSON *root = cJSON_Parse(json_start);
        if (root != NULL) {
            cJSON *report = cJSON_GetObjectItemCaseSensitive(root, "report");
            if (cJSON_IsString(report) && report->valuestring != NULL) {
                strlcpy(out, report->valuestring, out_len);
                cJSON_Delete(root);
                return;
            }
            cJSON_Delete(root);
        }
    }

    strlcpy(out, body, out_len);
}

static esp_err_t http_post_json(const bodyguard_event_t *event)
{
    char json[768];
    const risk_sample_t *risk = &event->trigger;
    int len = snprintf(json, sizeof(json),
                       "{\"event_id\":\"%s\",\"risk_pre\":%u,\"risk_final\":%u,\"confidence\":%u,"
                       "\"eeg\":{\"attention\":%u,\"fatigue\":%u,\"signal_quality\":%u},"
                       "\"gyro\":[%d,%d,%d],\"posture\":%u,\"timestamp\":%llu}",
                       event->node.event_id, risk->risk_pre, risk->risk_final, risk->confidence,
                       event->window.eeg_count ? event->window.eeg[event->window.eeg_count - 1].attention : 0,
                       event->window.eeg_count ? event->window.eeg[event->window.eeg_count - 1].fatigue : 0,
                       event->window.eeg_count ? event->window.eeg[event->window.eeg_count - 1].signal_quality : 0,
                       risk->gyro[0], risk->gyro[1], risk->gyro[2],
                       risk->pose_state,
                       (unsigned long long)risk->timestamp_ms);
    esp_http_client_config_t cfg = {
        .url = BODYGUARD_DATA_UPLOAD_URL,
        .timeout_ms = BODYGUARD_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, len);
    if (err == ESP_OK) {
        esp_http_client_write(client, json, len);
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        err = (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t http_post_ai_event(const bodyguard_event_t *event)
{
    char boundary[40];
    snprintf(boundary, sizeof(boundary), "----BG-%s", event->node.event_id);
    const sensor_sample_t *last_sensor = NULL;
    const eeg_sample_t *last_eeg = NULL;
    if (event->window.sensor_count > 0) {
        last_sensor = &event->window.sensor[event->window.sensor_count - 1];
    }
    if (event->window.eeg_count > 0) {
        last_eeg = &event->window.eeg[event->window.eeg_count - 1];
    }
    char sensor_json[768];
    int sensor_json_len = snprintf(
        sensor_json,
        sizeof(sensor_json),
        "{\"event_id\":\"%s\",\"risk\":{\"pre\":%u,\"final\":%u,\"confidence\":%u,"
        "\"pose_state\":%u,\"eeg_state\":%u,\"sys_state\":%u},"
        "\"mpu\":{\"angle\":%.1f,\"accel\":[%.3f,%.3f,%.3f],"
        "\"gyro\":[%.1f,%.1f,%.1f]},"
        "\"eeg\":{\"connected\":%u,\"attention\":%u,\"meditation\":%u,"
        "\"fatigue\":%u,\"signal_quality\":%u},"
        "\"series\":{\"sensor_count\":%u,\"eeg_count\":%u,\"risk_count\":%u,"
        "\"image_count\":%u,\"snapshot\":%u},\"timestamp\":%llu}",
        event->node.event_id,
        event->trigger.risk_pre,
        event->trigger.risk_final,
        event->trigger.confidence,
        event->trigger.pose_state,
        event->trigger.eeg_state,
        event->trigger.sys_state,
        last_sensor != NULL ? last_sensor->tilt_angle : 0.0f,
        last_sensor != NULL ? last_sensor->accel[0] : 0.0f,
        last_sensor != NULL ? last_sensor->accel[1] : 0.0f,
        last_sensor != NULL ? last_sensor->accel[2] : 0.0f,
        last_sensor != NULL ? last_sensor->gyro[0] : 0.0f,
        last_sensor != NULL ? last_sensor->gyro[1] : 0.0f,
        last_sensor != NULL ? last_sensor->gyro[2] : 0.0f,
        last_eeg != NULL && last_eeg->connected ? 1 : 0,
        last_eeg != NULL ? last_eeg->attention : 0,
        last_eeg != NULL ? last_eeg->relax : 0,
        last_eeg != NULL ? last_eeg->fatigue : 0,
        last_eeg != NULL ? last_eeg->signal_quality : 0,
        (unsigned)event->window.sensor_count,
        (unsigned)event->window.eeg_count,
        (unsigned)event->window.risk_count,
        (unsigned)event->window.image_count,
        event->window.snapshot.valid ? 1 : 0,
        (unsigned long long)event->trigger.timestamp_ms);
    if (sensor_json_len < 0) {
        sensor_json[0] = '\0';
        sensor_json_len = 0;
    } else if ((size_t)sensor_json_len >= sizeof(sensor_json)) {
        sensor_json_len = sizeof(sensor_json) - 1;
    }
    ESP_LOGI(TAG, "AI upload start event_id=%s images=%u snapshot=%u sensor=%u eeg=%u risk=%u",
             event->node.event_id,
             (unsigned)event->window.image_count,
             event->window.snapshot.valid ? 1 : 0,
             (unsigned)event->window.sensor_count,
             (unsigned)event->window.eeg_count,
             (unsigned)event->window.risk_count);

    esp_http_client_config_t cfg = {
        .url = BODYGUARD_AI_EVENT_UPLOAD_URL,
        .timeout_ms = BODYGUARD_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    char header[1024];
    size_t body_len = 0;
    size_t upload_images = event->window.image_count;
    if (upload_images > BODYGUARD_UPLOAD_MAX_IMAGES) {
        upload_images = BODYGUARD_UPLOAD_MAX_IMAGES;
    }
    for (size_t i = 0; i < upload_images; i++) {
        body_len += snprintf(header, sizeof(header),
                             "--%s\r\nContent-Disposition: form-data; name=\"images\"; filename=\"%s_%u.jpg\"\r\n"
                             "Content-Type: image/jpeg\r\n\r\n",
                             boundary, event->node.event_id, (unsigned)i);
        body_len += event->window.images[i].size + 2;
    }
    if (event->window.snapshot.valid) {
        body_len += snprintf(header, sizeof(header),
                             "--%s\r\nContent-Disposition: form-data; name=\"snapshot\"; filename=\"%s_snapshot.jpg\"\r\n"
                             "Content-Type: image/jpeg\r\n\r\n",
                             boundary, event->node.event_id);
        body_len += event->window.snapshot.size + 2;
    }
    body_len += snprintf(header, sizeof(header),
                         "--%s\r\nContent-Disposition: form-data; name=\"event_id\"\r\n\r\n%s\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n%s\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"sensor_json\"\r\n\r\n%s\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"risk_pre\"\r\n\r\n%u\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"risk_final\"\r\n\r\n%u\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"confidence\"\r\n\r\n%u\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"posture\"\r\n\r\n%u\r\n"
                         "--%s\r\nContent-Disposition: form-data; name=\"timestamp\"\r\n\r\n%llu\r\n"
                         "--%s--\r\n",
                         boundary, event->node.event_id,
                         boundary, AI_REPORT_PROMPT,
                         boundary, sensor_json,
                         boundary, event->trigger.risk_pre,
                         boundary, event->trigger.risk_final,
                         boundary, event->trigger.confidence,
                         boundary, event->trigger.pose_state,
                         boundary, (unsigned long long)event->trigger.timestamp_ms,
                         boundary);
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    for (size_t i = 0; i < upload_images; i++) {
        int hl = snprintf(header, sizeof(header),
                          "--%s\r\nContent-Disposition: form-data; name=\"images\"; filename=\"%s_%u.jpg\"\r\n"
                          "Content-Type: image/jpeg\r\n\r\n",
                          boundary, event->node.event_id, (unsigned)i);
        esp_http_client_write(client, header, hl);
        esp_http_client_write(client, (const char *)event->window.images[i].data,
                              (int)event->window.images[i].size);
        esp_http_client_write(client, "\r\n", 2);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (event->window.snapshot.valid) {
        int hl = snprintf(header, sizeof(header),
                          "--%s\r\nContent-Disposition: form-data; name=\"snapshot\"; filename=\"%s_snapshot.jpg\"\r\n"
                          "Content-Type: image/jpeg\r\n\r\n",
                          boundary, event->node.event_id);
        esp_http_client_write(client, header, hl);
        esp_http_client_write(client, (const char *)event->window.snapshot.data,
                              (int)event->window.snapshot.size);
        esp_http_client_write(client, "\r\n", 2);
    }

    int hl = snprintf(header, sizeof(header),
                      "--%s\r\nContent-Disposition: form-data; name=\"event_id\"\r\n\r\n%s\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n%s\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"sensor_json\"\r\n\r\n%s\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"risk_pre\"\r\n\r\n%u\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"risk_final\"\r\n\r\n%u\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"confidence\"\r\n\r\n%u\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"posture\"\r\n\r\n%u\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"timestamp\"\r\n\r\n%llu\r\n"
                      "--%s--\r\n",
                      boundary, event->node.event_id,
                      boundary, AI_REPORT_PROMPT,
                      boundary, sensor_json,
                      boundary, event->trigger.risk_pre,
                      boundary, event->trigger.risk_final,
                      boundary, event->trigger.confidence,
                      boundary, event->trigger.pose_state,
                      boundary, (unsigned long long)event->trigger.timestamp_ms,
                      boundary);
    esp_http_client_write(client, header, hl);

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char response[2048] = {0};
    int read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
    uint8_t ai = 0;
    if (status >= 200 && status < 300 && read_len > 0) {
        (void)temporal_learning_on_ai_response(event->node.event_id, response);
        char ai_report[AI_REPORT_LEN];
        char report_src[AI_REPORT_LEN];
        ai = parse_ai_result_json(response, report_src, sizeof(report_src));
        if (report_src[0] == '\0') {
            ai_report_payload(response, report_src, sizeof(report_src));
        }
        copy_utf8_chars(ai_report, sizeof(ai_report), report_src, 30);
        ESP_LOGI(TAG, "AI response parsed event_id=%s status=%d len=%d ai=%u report=%s",
                 event->node.event_id, status, read_len, ai, ai_report);
        if (ai != 0) {
            risk_engine_apply_ai_report(event->node.event_id, ai, ai_report);
        } else {
            ESP_LOGW(TAG, "AI response has no valid ai_result event_id=%s body=%.160s",
                     event->node.event_id, response);
        }
    } else {
        ESP_LOGW(TAG, "AI response empty or failed event_id=%s status=%d len=%d",
                 event->node.event_id, status, read_len);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t post_ai_event_with_retry(const bodyguard_event_t *event)
{
    for (int attempt = 0; attempt <= BODYGUARD_AI_HTTP_RETRY; attempt++) {
        esp_err_t err = http_post_ai_event(event);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt < BODYGUARD_AI_HTTP_RETRY) {
            upload_delay_ms(1500);
        }
    }
    return ESP_FAIL;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_ctx.event_group, WIFI_CONNECTED_BIT);
            const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected reason=%u, reconnecting",
                     disc != NULL ? disc->reason : 0);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event != NULL) {
            s_ctx.ip4_addr = event->ip_info.ip.addr;
            ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Live video: http://" IPSTR ":%d/stream",
                     IP2STR(&event->ip_info.ip), BODYGUARD_MJPEG_STREAM_PORT);
        }
        xEventGroupSetBits(s_ctx.event_group, WIFI_CONNECTED_BIT);
    }
}

static void upload_delay_ms(uint32_t delay_ms)
{
    uint32_t remaining = delay_ms;
    while (remaining > 0) {
        uint32_t slice = remaining > 1000 ? 1000 : remaining;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
    }
}

static void upload_task(void *arg)
{
    (void)arg;
    static const uint32_t retry_ms[] = {BODYGUARD_UPLOAD_RETRY_INTERVALS_MS};
    bodyguard_event_t *event = heap_caps_calloc(1, sizeof(bodyguard_event_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event == NULL) {
        event = calloc(1, sizeof(bodyguard_event_t));
    }
    if (event == NULL) {
        ESP_LOGE(TAG, "upload event buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "task_wifi_upload started");

    while (true) {
        memset(event, 0, sizeof(*event));
        if (!event_queue_pop_for_upload(event, BODYGUARD_WIFI_TASK_IDLE_MS)) {
            esp_task_wdt_reset();
            continue;
        }

        bool done = false;
        uint8_t start_attempt = event->node.retry_count;
        for (uint8_t attempt = start_attempt; attempt < BODYGUARD_UPLOAD_MAX_RETRY; attempt++) {
            event->node.retry_count = attempt + 1;
            event_queue_update_status(&event->node);

            if (!wifi_upload_is_connected()) {
                ESP_LOGW(TAG, "network offline, postpone upload event_id=%s", event->node.event_id);
                upload_delay_ms(retry_ms[attempt < 5 ? attempt : 4]);
                continue;
            }

            esp_err_t image_err = post_ai_event_with_retry(event);
            event->node.image_status = (image_err == ESP_OK);
            esp_err_t json_err = http_post_json(event);
            event->node.json_status = (json_err == ESP_OK);
            event_queue_update_status(&event->node);

            if (image_err == ESP_OK && json_err == ESP_OK) {
                done = true;
                event_queue_mark_uploaded(event->node.event_id);
                ESP_LOGI(TAG, "upload complete event_id=%s", event->node.event_id);
                break;
            }

            ESP_LOGW(TAG, "upload failed event_id=%s attempt=%u image=%s json=%s",
                     event->node.event_id, attempt + 1,
                     esp_err_to_name(image_err), esp_err_to_name(json_err));
            upload_delay_ms(retry_ms[attempt < 5 ? attempt : 4]);
        }

        if (!done) {
            ESP_LOGE(TAG, "upload retries exhausted, keep cached event_id=%s", event->node.event_id);
        }
        event_queue_release_payload(event);
        esp_task_wdt_reset();
    }
}

esp_err_t wifi_upload_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.event_group = xEventGroupCreate();
    if (s_ctx.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return loop;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        ip_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, BODYGUARD_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, BODYGUARD_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t wifi_upload_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "wifi_upload is not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(upload_task, "task_wifi_upload", BODYGUARD_TASK_STACK_HUGE, NULL,
                                BODYGUARD_TASK_PRIO_WIFI, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wifi_upload_is_connected(void)
{
    if (s_ctx.event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_ctx.event_group) & WIFI_CONNECTED_BIT) != 0;
}

uint32_t wifi_upload_get_ip4_addr(void)
{
    return s_ctx.ip4_addr;
}
