/**
 * @file app_state.c
 * @brief Shared packet state for the T5 safety monitor display terminal.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "data_def.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */
static MUTEX_HANDLE s_state_lock = NULL;
static t5_app_state_t s_state = {0};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Check whether one byte buffer is valid UTF-8 text.
 * @param[in] text Text buffer to check.
 * @param[in] max_len Maximum byte count to scan.
 * @return true if text is valid UTF-8, otherwise false.
 */
static bool __state_is_valid_utf8(const char *text, size_t max_len)
{
    size_t i = 0;

    if (text == NULL) {
        return false;
    }

    while (i < max_len && text[i] != '\0') {
        uint8_t c = (uint8_t)text[i];
        size_t need = 0;
        uint32_t codepoint = 0;

        if (c <= 0x7F) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            codepoint = (uint32_t)(c & 0x1F);
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            codepoint = (uint32_t)(c & 0x0F);
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            codepoint = (uint32_t)(c & 0x07);
        } else {
            return false;
        }

        if (i + need >= max_len) {
            return false;
        }

        for (size_t j = 1; j <= need; j++) {
            uint8_t cc = (uint8_t)text[i + j];

            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (uint32_t)(cc & 0x3F);
        }

        if ((need == 2 && codepoint < 0x0800) ||
            (need == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint > 0x10FFFF) {
            return false;
        }

        i += need + 1;
    }

    return true;
}

/**
 * @brief Keep a fixed-size text field ending at a valid UTF-8 boundary.
 * @param[in,out] text Text field to sanitize.
 * @param[in] max_len Field byte capacity.
 * @param[in] fallback Fallback text when the field is empty or invalid.
 * @return none
 */
static void __state_sanitize_utf8_field(char *text, size_t max_len, const char *fallback)
{
    if (text == NULL || max_len == 0 || fallback == NULL) {
        return;
    }

    text[max_len - 1] = '\0';
    if (text[0] == '\0') {
        strncpy(text, fallback, max_len - 1);
        text[max_len - 1] = '\0';
        return;
    }

    if (__state_is_valid_utf8(text, max_len)) {
        return;
    }

    T5_LOG_E("invalid UTF-8 text field, use fallback");
    memset(text, 0, max_len);
    strncpy(text, fallback, max_len - 1);
    text[max_len - 1] = '\0';
}

/**
 * @brief 初始化默认包，保证未收到 BLE 数据时 UI 也有稳定内容。
 * @return none
 */
static void __state_set_default_packet(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.packet.head = BLE_HEAD;
    s_state.packet.tail = BLE_TAIL;
    s_state.packet.sys_state = T5_SYS_STANDBY;
    s_state.packet.risk_score = 0;
    s_state.packet.eeg_state = 0;
    s_state.packet.pose_state = 0;
    s_state.packet.ble_conn_state = 0;
    s_state.packet.cloud_conn_state = 0;
    s_state.packet.posture_weight = 0.0f;
    s_state.packet.eeg_weight = 0.0f;
    s_state.packet.burst_weight = 0.0f;
    s_state.packet.warning_threshold = 70;
    s_state.packet.danger_threshold = 90;
    s_state.packet.learning_enabled = 0;
    s_state.learning_valid = false;
    strncpy(s_state.packet.ai_report, "等待AI分析...", sizeof(s_state.packet.ai_report) - 1);
    strncpy(s_state.packet.event_id, "-", sizeof(s_state.packet.event_id) - 1);
}

/**
 * @brief Clamp packet enum fields to UI-safe ranges.
 * @param[in,out] packet Packet to sanitize.
 * @return none
 */
static void __state_sanitize_packet(ble_t5_packet_t *packet)
{
    if (packet->sys_state > T5_SYS_EMERGENCY) {
        packet->sys_state = T5_SYS_STANDBY;
    }
    if (packet->risk_score > 100) {
        packet->risk_score = 100;
    }
    if (packet->eeg_state > 3) {
        packet->eeg_state = 3;
    }
    if (packet->pose_state > 3) {
        packet->pose_state = 3;
    }
    if (packet->posture_weight < 0.0f) {
        packet->posture_weight = 0.0f;
    }
    if (packet->posture_weight > 1.0f) {
        packet->posture_weight = 1.0f;
    }
    if (packet->eeg_weight < 0.0f) {
        packet->eeg_weight = 0.0f;
    }
    if (packet->eeg_weight > 1.0f) {
        packet->eeg_weight = 1.0f;
    }
    if (packet->burst_weight < 0.0f) {
        packet->burst_weight = 0.0f;
    }
    if (packet->burst_weight > 1.0f) {
        packet->burst_weight = 1.0f;
    }
    if (packet->warning_threshold > 100) {
        packet->warning_threshold = 100;
    }
    if (packet->danger_threshold > 100) {
        packet->danger_threshold = 100;
    }

    __state_sanitize_utf8_field(packet->ai_report, AI_REPORT_LEN, "等待AI分析...");
    __state_sanitize_utf8_field(packet->event_id, EVENT_ID_LEN, "-");
}

OPERATE_RET app_state_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (s_state_lock != NULL) {
        return OPRT_OK;
    }

    rt = tal_mutex_create_init(&s_state_lock);
    if (rt != OPRT_OK) {
        return rt;
    }
    tal_mutex_lock(s_state_lock);
    __state_set_default_packet();
    tal_mutex_unlock(s_state_lock);
    return OPRT_OK;
}

OPERATE_RET app_state_update_packet(const ble_t5_packet_t *packet)
{
    return app_state_update_packet_ex(packet, true);
}

OPERATE_RET app_state_update_packet_ex(const ble_t5_packet_t *packet, bool learning_valid)
{
    ble_t5_packet_t local_packet;

    if (packet == NULL || s_state_lock == NULL) {
        return OPRT_INVALID_PARM;
    }

    memcpy(&local_packet, packet, sizeof(local_packet));
    __state_sanitize_packet(&local_packet);

    tal_mutex_lock(s_state_lock);
    memcpy(&s_state.packet, &local_packet, sizeof(s_state.packet));
    s_state.packet_valid = true;
    s_state.learning_valid = learning_valid;
    s_state.ble_connected = true;
    s_state.ble_scanning = false;
    s_state.ble_connecting = false;
    s_state.packet.ble_conn_state = 1;
    s_state.packet_seq++;
    s_state.last_packet_ms = tal_system_get_millisecond();
    tal_mutex_unlock(s_state_lock);
    return OPRT_OK;
}

void app_state_set_ble_status(bool connected, bool scanning, bool connecting)
{
    if (s_state_lock == NULL) {
        return;
    }

    tal_mutex_lock(s_state_lock);
    s_state.ble_connected = connected;
    s_state.ble_scanning = scanning;
    s_state.ble_connecting = connecting;
    s_state.packet.ble_conn_state = connected ? 1 : 0;
    tal_mutex_unlock(s_state_lock);
}

OPERATE_RET app_state_get(t5_app_state_t *state)
{
    if (state == NULL || s_state_lock == NULL) {
        return OPRT_INVALID_PARM;
    }

    tal_mutex_lock(s_state_lock);
    memcpy(state, &s_state, sizeof(*state));
    tal_mutex_unlock(s_state_lock);
    return OPRT_OK;
}

bool app_state_decode_packet(const uint8_t *data, size_t len, ble_t5_packet_t *packet)
{
    uint16_t legacy_tail;
    uint16_t packet_head = 0;
    uint16_t packet_tail = 0;
    uint16_t learning_v1_tail = 0;
    uint8_t raw_sys_state = 0;

    if (data == NULL || packet == NULL) {
        return false;
    }

    if (len >= sizeof(packet_head)) {
        memcpy(&packet_head, data, sizeof(packet_head));
    }
    if (len >= 3) {
        raw_sys_state = data[2];
    }
    if (len >= sizeof(packet_tail)) {
        memcpy(&packet_tail, data + len - sizeof(packet_tail), sizeof(packet_tail));
    }

    if (len == sizeof(ble_t5_packet_t) && len == BODYGUARD_PACKET_SIZE) {
        memcpy(packet, data, sizeof(*packet));
        if (packet->head != BLE_HEAD || packet->tail != BLE_TAIL) {
            T5_LOG_E("BLE packet invalid len=%u head=0x%04x tail=0x%04x sys=%u",
                     (unsigned)len, (unsigned)packet->head, (unsigned)packet->tail, (unsigned)packet->sys_state);
            return false;
        }
        __state_sanitize_packet(packet);
        return true;
    }

    if (len == BODYGUARD_LEARNING_V1_PACKET_SIZE) {
        memset(packet, 0, sizeof(*packet));
        memcpy(packet, data, BODYGUARD_LEARNING_V1_PACKET_SIZE - sizeof(uint16_t));
        memcpy(&learning_v1_tail,
               data + BODYGUARD_LEARNING_V1_PACKET_SIZE - sizeof(uint16_t),
               sizeof(learning_v1_tail));
        if (packet->head != BLE_HEAD || learning_v1_tail != BLE_TAIL) {
            T5_LOG_E("BLE v1 packet invalid len=%u head=0x%04x tail=0x%04x sys=%u",
                     (unsigned)len, (unsigned)packet->head, (unsigned)learning_v1_tail,
                     (unsigned)packet->sys_state);
            return false;
        }

        packet->tail = BLE_TAIL;
        packet->eeg_attention = 0;
        packet->eeg_meditation = 0;
        packet->eeg_fatigue = 0;
        packet->eeg_signal_quality = 0;
        packet->eeg_connected = 0;
        packet->eeg_score = 0;
        __state_sanitize_packet(packet);
        return true;
    }

    if (len != BODYGUARD_LEGACY_PACKET_SIZE) {
        T5_LOG_E("BLE packet unsupported len=%u head=0x%04x tail=0x%04x sys=%u",
                 (unsigned)len, (unsigned)packet_head, (unsigned)packet_tail, (unsigned)raw_sys_state);
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    memcpy(packet, data, BODYGUARD_LEGACY_PACKET_SIZE - sizeof(uint16_t));
    memcpy(&legacy_tail, data + BODYGUARD_LEGACY_PACKET_SIZE - sizeof(uint16_t), sizeof(legacy_tail));
    if (packet->head != BLE_HEAD || legacy_tail != BLE_TAIL) {
        T5_LOG_E("BLE legacy packet invalid len=%u head=0x%04x tail=0x%04x sys=%u",
                 (unsigned)len, (unsigned)packet->head, (unsigned)legacy_tail, (unsigned)packet->sys_state);
        return false;
    }

    packet->tail = BLE_TAIL;
    packet->posture_weight = 0.0f;
    packet->eeg_weight = 0.0f;
    packet->burst_weight = 0.0f;
    packet->warning_threshold = 70;
    packet->danger_threshold = 90;
    packet->learning_enabled = 0;
    packet->eeg_attention = 0;
    packet->eeg_meditation = 0;
    packet->eeg_fatigue = 0;
    packet->eeg_signal_quality = 0;
    packet->eeg_connected = 0;
    packet->eeg_score = 0;
    __state_sanitize_packet(packet);
    return true;
}
