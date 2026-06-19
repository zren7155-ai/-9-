#include "learning_storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "learning_storage";
static const char *KEY_PARAMS = "learn_param";
static const char *KEY_AI_CACHE = "learn_ai";

esp_err_t learning_storage_init(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BODYGUARD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "learning NVS open failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t learning_storage_load_params(adaptive_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BODYGUARD_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*params);
    err = nvs_get_blob(handle, KEY_PARAMS, params, &len);
    nvs_close(handle);
    if (err == ESP_OK && len != sizeof(*params)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t learning_storage_save_params(const adaptive_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BODYGUARD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, KEY_PARAMS, params, sizeof(*params));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save adaptive params failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t learning_storage_cache_ai_feedback(const learning_ai_feedback_t *feedback)
{
    if (feedback == NULL || feedback->event_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(BODYGUARD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    /* 只缓存最近一次 AI 学习反馈，避免 NVS 被高频事件写满。 */
    err = nvs_set_blob(handle, KEY_AI_CACHE, feedback, sizeof(*feedback));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
