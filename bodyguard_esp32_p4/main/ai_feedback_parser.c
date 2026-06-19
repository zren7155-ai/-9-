#include "ai_feedback_parser.h"

#include <string.h>

#include "cJSON.h"

static void copy_json_string(cJSON *root, const char *name, char *dst, size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL && dst_len > 0) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

static bool copy_json_float(cJSON *root, const char *name, float *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item)) {
        *dst = (float)item->valuedouble;
        return true;
    }
    return false;
}

esp_err_t ai_feedback_parser_parse(const char *json, learning_ai_feedback_t *out)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *start = strchr(json, '{');
    if (start == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_Parse(start);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    copy_json_string(root, "event_id", out->event_id, sizeof(out->event_id));
    copy_json_string(root, "danger_type", out->danger_type, sizeof(out->danger_type));
    copy_json_string(root, "recommendation", out->recommendation, sizeof(out->recommendation));
    (void)copy_json_float(root, "confidence", &out->confidence);
    (void)copy_json_float(root, "pose_reliable", &out->pose_reliable);
    (void)copy_json_float(root, "eeg_reliable", &out->eeg_reliable);
    out->has_pose_weight = copy_json_float(root, "recommended_pose_weight",
                                           &out->recommended_pose_weight);
    out->has_eeg_weight = copy_json_float(root, "recommended_eeg_weight",
                                          &out->recommended_eeg_weight);

    cJSON *false_alarm = cJSON_GetObjectItemCaseSensitive(root, "false_alarm");
    out->false_alarm = cJSON_IsTrue(false_alarm);

    cJSON_Delete(root);
    return out->event_id[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}
