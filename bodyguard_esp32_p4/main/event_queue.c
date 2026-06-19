#include "event_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sys/stat.h"
#include "unistd.h"

static const char *TAG = "event_queue";

static QueueHandle_t s_upload_queue;
static nvs_handle_t s_nvs;
static char s_current_event_id[BODYGUARD_EVENT_ID_STR_LEN] = "BOOT";
static bool s_sd_mounted;

uint64_t bodyguard_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

void event_id_generate(char *buf, size_t buf_len)
{
    uint64_t now = bodyguard_now_ms();
    uint16_t rnd = (uint16_t)(esp_random() & 0xFFFF);
    snprintf(buf, buf_len, "%llu-%04X", (unsigned long long)now, rnd);
}

uint8_t ble_packet_checksum(const ble_packet_t *packet)
{
    const uint8_t *p = (const uint8_t *)packet;
    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(ble_packet_t) - sizeof(packet->checksum); i++) {
        sum ^= p[i];
    }
    return sum;
}

static void event_node_key(const char *event_id, char *key, size_t key_len)
{
    uint32_t h = 2166136261u;
    for (const char *p = event_id; *p; ++p) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    snprintf(key, key_len, "%s%08lX", BODYGUARD_NVS_EVENT_PREFIX, (unsigned long)h);
}

static void persist_node(const event_node_t *node)
{
    if (s_nvs == 0 || node == NULL) {
        return;
    }

    char key[16];
    event_node_key(node->event_id, key, sizeof(key));
    esp_err_t err = nvs_set_blob(s_nvs, key, node, sizeof(*node));
    if (err == ESP_OK) {
        nvs_commit(s_nvs);
    } else {
        ESP_LOGW(TAG, "NVS event status write failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t mount_tf_card(void)
{
    /*
     * 当前 ESP32-P4 工程使用 ESP-Hosted WiFi。实测把 SDSPI/SDMMC 组件接入后，
     * hosted SDIO 在 app_main 前创建 mempool 会失败并循环重启。
     * 为保证比赛主链路稳定，TF 主存储临时降级为 NVS 状态缓存；事件图片仍会上传云端。
     * 后续若要恢复 TF，需要在不影响 hosted SDIO 内存池的配置下单独验证。
     */
    s_sd_mounted = false;
    ESP_LOGW(TAG, "TF storage disabled for hosted-WiFi stability, using NVS fallback");
    return ESP_ERR_NOT_SUPPORTED;
}

static void cache_event_to_tf(const bodyguard_event_t *event)
{
    if (!s_sd_mounted || event == NULL) {
        return;
    }

    char dir[160];
    char path[192];
    snprintf(dir, sizeof(dir), "%s/%s", BODYGUARD_EVENT_CACHE_DIR, event->node.event_id);
    if (mkdir(dir, 0775) != 0) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "event dir create failed: %s", dir);
            return;
        }
    }

    snprintf(path, sizeof(path), "%s/meta.txt", dir);
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGW(TAG, "event meta write failed: %s", path);
        return;
    }
    fprintf(fp, "event_id=%s\nrisk_pre=%u\ntimestamp=%llu\nimage_count=%u\n",
            event->node.event_id, event->trigger.risk_pre,
            (unsigned long long)event->trigger.timestamp_ms,
            (unsigned)event->window.image_count);
    fclose(fp);

    snprintf(path, sizeof(path), "%s/eeg.csv", dir);
    fp = fopen(path, "w");
    if (fp != NULL) {
        fprintf(fp, "timestamp_ms,attention,fatigue,relax,signal_quality,connected\n");
        for (size_t i = 0; i < event->window.eeg_count; i++) {
            const eeg_sample_t *sample = &event->window.eeg[i];
            fprintf(fp, "%llu,%u,%u,%u,%u,%u\n",
                    (unsigned long long)sample->timestamp_ms,
                    sample->attention,
                    sample->fatigue,
                    sample->relax,
                    sample->signal_quality,
                    sample->connected ? 1 : 0);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "eeg series write failed: %s", path);
    }

    snprintf(path, sizeof(path), "%s/sensor.csv", dir);
    fp = fopen(path, "w");
    if (fp != NULL) {
        fprintf(fp, "timestamp_ms,accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z,tilt_angle,angle_delta,posture\n");
        for (size_t i = 0; i < event->window.sensor_count; i++) {
            const sensor_sample_t *sample = &event->window.sensor[i];
            fprintf(fp, "%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%u\n",
                    (unsigned long long)sample->timestamp_ms,
                    sample->accel[0], sample->accel[1], sample->accel[2],
                    sample->gyro[0], sample->gyro[1], sample->gyro[2],
                    sample->tilt_angle, sample->angle_delta, sample->posture);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "sensor series write failed: %s", path);
    }

    snprintf(path, sizeof(path), "%s/risk.csv", dir);
    fp = fopen(path, "w");
    if (fp != NULL) {
        fprintf(fp, "timestamp_ms,pose_score,eeg_score,burst_score,confidence,risk_pre,risk_final,eeg_state,pose_state,sys_state,ai_result\n");
        for (size_t i = 0; i < event->window.risk_count; i++) {
            const risk_sample_t *sample = &event->window.risk[i];
            fprintf(fp, "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                    (unsigned long long)sample->timestamp_ms,
                    sample->pose_score,
                    sample->eeg_score,
                    sample->burst_score,
                    sample->confidence,
                    sample->risk_pre,
                    sample->risk_final,
                    sample->eeg_state,
                    sample->pose_state,
                    sample->sys_state,
                    sample->ai_result);
        }
        fclose(fp);
    } else {
        ESP_LOGW(TAG, "risk series write failed: %s", path);
    }

    if (event->window.snapshot.valid && event->window.snapshot.data != NULL) {
        snprintf(path, sizeof(path), "%s/snapshot.jpg", dir);
        fp = fopen(path, "wb");
        if (fp != NULL) {
            fwrite(event->window.snapshot.data, 1, event->window.snapshot.size, fp);
            fclose(fp);
        } else {
            ESP_LOGW(TAG, "snapshot write failed: %s", path);
        }
    }

    for (size_t i = 0; i < event->window.image_count; i++) {
        const camera_frame_t *frame = &event->window.images[i];
        if (!frame->valid || frame->data == NULL || frame->size == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/frame_%03u.jpg", dir, (unsigned)i);
        fp = fopen(path, "wb");
        if (fp == NULL) {
            ESP_LOGW(TAG, "event frame write failed: %s", path);
            continue;
        }
        fwrite(frame->data, 1, frame->size, fp);
        fclose(fp);
    }
}

static bool clone_frame(camera_frame_t *dst, const camera_frame_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src == NULL || !src->valid || src->data == NULL || src->size == 0) {
        return true;
    }

    dst->data = heap_caps_malloc(src->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst->data == NULL) {
        dst->data = malloc(src->size);
    }
    if (dst->data == NULL) {
        return false;
    }
    memcpy(dst->data, src->data, src->size);
    dst->size = src->size;
    dst->timestamp_ms = src->timestamp_ms;
    dst->valid = true;
    dst->owned = true;
    return true;
}

static bool clone_event_for_queue(bodyguard_event_t *dst, const bodyguard_event_t *src)
{
    *dst = *src;
    dst->cached = true;
    dst->window.snapshot.data = NULL;
    for (size_t i = 0; i < BODYGUARD_CAMERA_RING_FRAMES; i++) {
        dst->window.images[i].data = NULL;
    }

    for (size_t i = 0; i < src->window.image_count; i++) {
        if (!clone_frame(&dst->window.images[i], &src->window.images[i])) {
            event_queue_release_payload(dst);
            return false;
        }
    }
    if (!clone_frame(&dst->window.snapshot, &src->window.snapshot)) {
        event_queue_release_payload(dst);
        return false;
    }
    return true;
}

static bodyguard_event_t *alloc_event_buffer(void)
{
    bodyguard_event_t *event = heap_caps_calloc(1, sizeof(bodyguard_event_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event == NULL) {
        event = calloc(1, sizeof(bodyguard_event_t));
    }
    return event;
}

esp_err_t event_queue_init(void)
{
    s_upload_queue = xQueueCreate(BODYGUARD_EVENT_QUEUE_DEPTH, sizeof(bodyguard_event_t));
    if (s_upload_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_open(BODYGUARD_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, upload state fallback disabled: %s", esp_err_to_name(err));
        s_nvs = 0;
    }
    ESP_LOGI(TAG, "event queue ready depth=%d sdmmc=%d",
             BODYGUARD_EVENT_QUEUE_DEPTH, s_sd_mounted ? 1 : 0);
    return ESP_OK;
}

esp_err_t event_queue_mount_storage(void)
{
    return mount_tf_card();
}

esp_err_t event_queue_push(const bodyguard_event_t *event)
{
    if (event == NULL || s_upload_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    persist_node(&event->node);
    cache_event_to_tf(event);
    event_queue_set_current_event_id(event->node.event_id);

    bodyguard_event_t *queued = alloc_event_buffer();
    if (queued == NULL) {
        ESP_LOGE(TAG, "event queue clone buffer alloc failed event_id=%s", event->node.event_id);
        return ESP_ERR_NO_MEM;
    }

    if (!clone_event_for_queue(queued, event)) {
        ESP_LOGE(TAG, "event payload clone failed event_id=%s", event->node.event_id);
        free(queued);
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_upload_queue, queued, 0) != pdTRUE) {
        bodyguard_event_t *drop = alloc_event_buffer();
        if (drop != NULL && xQueueReceive(s_upload_queue, drop, 0) == pdTRUE) {
            event_queue_release_payload(drop);
        }
        if (drop != NULL) {
            free(drop);
        }
        if (xQueueSend(s_upload_queue, queued, 0) != pdTRUE) {
            event_queue_release_payload(queued);
            free(queued);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "upload queue full, dropped oldest event");
    }

    ESP_LOGI(TAG, "event queued event_id=%s risk=%u",
             event->node.event_id, event->trigger.risk_pre);
    free(queued);
    return ESP_OK;
}

void event_queue_release_payload(bodyguard_event_t *event)
{
    if (event == NULL) {
        return;
    }

    for (size_t i = 0; i < event->window.image_count; i++) {
        if (event->cached || event->window.images[i].owned) {
            heap_caps_free(event->window.images[i].data);
        }
        event->window.images[i].data = NULL;
        event->window.images[i].size = 0;
        event->window.images[i].valid = false;
        event->window.images[i].owned = false;
    }
    if (event->cached || event->window.snapshot.owned) {
        heap_caps_free(event->window.snapshot.data);
    }
    event->window.snapshot.data = NULL;
    event->window.snapshot.size = 0;
    event->window.snapshot.valid = false;
    event->window.snapshot.owned = false;
}

bool event_queue_pop_for_upload(bodyguard_event_t *event, uint32_t timeout_ms)
{
    if (s_upload_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(s_upload_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void event_queue_update_status(const event_node_t *node)
{
    persist_node(node);
}

void event_queue_mark_uploaded(const char *event_id)
{
    if (event_id == NULL) {
        return;
    }

    if (s_nvs != 0) {
        char key[16];
        event_node_key(event_id, key, sizeof(key));
        nvs_erase_key(s_nvs, key);
        nvs_commit(s_nvs);
    }

    if (s_sd_mounted) {
        char path[160];
        snprintf(path, sizeof(path), "%s/%s/meta.txt", BODYGUARD_EVENT_CACHE_DIR, event_id);
        unlink(path);
        snprintf(path, sizeof(path), "%s/%s/eeg.csv", BODYGUARD_EVENT_CACHE_DIR, event_id);
        unlink(path);
        snprintf(path, sizeof(path), "%s/%s/sensor.csv", BODYGUARD_EVENT_CACHE_DIR, event_id);
        unlink(path);
        snprintf(path, sizeof(path), "%s/%s/risk.csv", BODYGUARD_EVENT_CACHE_DIR, event_id);
        unlink(path);
        snprintf(path, sizeof(path), "%s/%s/snapshot.jpg", BODYGUARD_EVENT_CACHE_DIR, event_id);
        unlink(path);
        for (size_t i = 0; i < BODYGUARD_CAMERA_RING_FRAMES; i++) {
            snprintf(path, sizeof(path), "%s/%s/frame_%03u.jpg",
                     BODYGUARD_EVENT_CACHE_DIR, event_id, (unsigned)i);
            unlink(path);
        }
        snprintf(path, sizeof(path), "%s/%s", BODYGUARD_EVENT_CACHE_DIR, event_id);
        rmdir(path);
    }
}

const char *event_queue_current_event_id(void)
{
    return s_current_event_id;
}

void event_queue_set_current_event_id(const char *event_id)
{
    if (event_id == NULL || event_id[0] == '\0') {
        return;
    }
    strlcpy(s_current_event_id, event_id, sizeof(s_current_event_id));
}
