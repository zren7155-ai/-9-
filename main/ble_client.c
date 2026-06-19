/**
 * @file ble_client.c
 * @brief BLE central/client for connecting BodyGuard-P4 and receiving notify packets.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "data_def.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define BLE_SCAN_INTERVAL       0x0400
#define BLE_SCAN_WINDOW         0x0400
#define BLE_CONN_INTERVAL_MIN   0x0018
#define BLE_CONN_INTERVAL_MAX   0x0030
#define BLE_CONN_TIMEOUT        0x0100
#define BLE_CONNECT_TIMEOUT_MS  5000
#define BLE_CLIENT_MTU          247
#define BLE_ADV_CACHE_SIZE      8

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    bool used;
    bool name_seen;
    bool service_seen;
    TAL_BLE_ADDR_T addr;
} bodyguard_adv_cache_t;

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */
static TIMER_ID s_retry_timer = NULL;
static TIMER_ID s_timeout_timer = NULL;
static bool s_ready = false;
static bool s_scanning = false;
static bool s_connecting = false;
static bool s_connected = false;
static uint8_t s_retry_count = 0;
static TAL_BLE_PEER_INFO_T s_peer = {0};
static bodyguard_adv_cache_t s_adv_cache[BLE_ADV_CACHE_SIZE] = {0};

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */
static OPERATE_RET __ble_start_scan(void);

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief 从广播 TLV 中查找完整本地名称或短名称。
 * @param[in] data 广播数据。
 * @param[in] len 广播数据长度。
 * @return true=名称为 BodyGuard-P4，false=不是目标设备。
 */
static bool __addr_equal(const TAL_BLE_ADDR_T *left, const TAL_BLE_ADDR_T *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    return left->type == right->type && memcmp(left->addr, right->addr, sizeof(left->addr)) == 0;
}

/**
 * @brief Find or allocate an advertisement cache slot for one peer address.
 * @param[in] addr Peer address from scan report.
 * @return Cache slot pointer, or NULL if no slot is available.
 */
static bodyguard_adv_cache_t *__adv_cache_get(const TAL_BLE_ADDR_T *addr)
{
    uint8_t i;
    bodyguard_adv_cache_t *free_slot = NULL;

    if (addr == NULL) {
        return NULL;
    }

    for (i = 0; i < BLE_ADV_CACHE_SIZE; i++) {
        if (s_adv_cache[i].used && __addr_equal(&s_adv_cache[i].addr, addr)) {
            return &s_adv_cache[i];
        }
        if (!s_adv_cache[i].used && free_slot == NULL) {
            free_slot = &s_adv_cache[i];
        }
    }

    if (free_slot == NULL) {
        free_slot = &s_adv_cache[0];
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    memcpy(&free_slot->addr, addr, sizeof(free_slot->addr));
    return free_slot;
}

/**
 * @brief Find BodyGuard-P4 local name in BLE advertisement TLV data.
 * @param[in] data Advertisement data.
 * @param[in] len Advertisement data length.
 * @return true if the target name is present.
 */
static bool __adv_name_match(const uint8_t *data, uint8_t len)
{
    uint8_t offset = 0;
    const char *target = BODYGUARD_ADV_NAME;
    size_t target_len = strlen(target);

    if (data == NULL) {
        return false;
    }

    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        uint8_t field_type;
        uint8_t value_len;

        if (field_len == 0 || offset + field_len >= len) {
            break;
        }

        field_type = data[offset + 1];
        value_len = (uint8_t)(field_len - 1);
        if ((field_type == 0x08 || field_type == 0x09) && value_len == target_len &&
            memcmp(&data[offset + 2], target, target_len) == 0) {
            return true;
        }

        offset = (uint8_t)(offset + field_len + 1);
    }

    return false;
}

/**
 * @brief Find service UUID 0xFD50 in BLE advertisement TLV data.
 * @param[in] data Advertisement data.
 * @param[in] len Advertisement data length.
 * @return true if the target service UUID is present.
 */
static bool __adv_service_match(const uint8_t *data, uint8_t len)
{
    uint8_t offset = 0;

    if (data == NULL) {
        return false;
    }

    while (offset + 1 < len) {
        uint8_t field_len = data[offset];
        uint8_t field_type;
        uint8_t value_len;
        uint8_t i;

        if (field_len == 0 || offset + field_len >= len) {
            break;
        }

        field_type = data[offset + 1];
        value_len = (uint8_t)(field_len - 1);
        if (field_type == 0x02 || field_type == 0x03) {
            for (i = 0; i + 1 < value_len; i += 2) {
                uint16_t uuid16 = (uint16_t)data[offset + 2 + i] |
                                  ((uint16_t)data[offset + 3 + i] << 8);
                if (uuid16 == BODYGUARD_SERVICE_UUID) {
                    return true;
                }
            }
        }

        offset = (uint8_t)(offset + field_len + 1);
    }

    return false;
}

/**
 * @brief 连接扫描到的 BodyGuard-P4 外设。
 * @param[in] addr 外设地址。
 * @return none
 */
static void __connect_peer(const TAL_BLE_ADDR_T *addr)
{
    TAL_BLE_CONN_PARAMS_T conn_cfg;
    TAL_BLE_PEER_INFO_T peer;
    OPERATE_RET rt;
    uint8_t i;

    if (addr == NULL || s_connecting || s_connected) {
        return;
    }

    (void)tal_ble_scan_stop();
    s_scanning = false;
    app_state_set_ble_status(false, false, true);

    memset(&peer, 0, sizeof(peer));
    peer.peer_addr.type = addr->type;
    for (i = 0; i < sizeof(peer.peer_addr.addr); i++) {
        peer.peer_addr.addr[i] = addr->addr[sizeof(peer.peer_addr.addr) - 1 - i];
    }

    memset(&conn_cfg, 0, sizeof(conn_cfg));
    conn_cfg.min_conn_interval = BLE_CONN_INTERVAL_MIN;
    conn_cfg.max_conn_interval = BLE_CONN_INTERVAL_MAX;
    conn_cfg.latency = 0;
    conn_cfg.conn_sup_timeout = BLE_CONN_TIMEOUT;
    conn_cfg.connection_timeout = BLE_CONNECT_TIMEOUT_MS;

    s_connecting = true;
    rt = tal_ble_connect_and_discovery(peer, &conn_cfg);
    if (rt != OPRT_OK) {
        T5_LOG_E("connect BodyGuard-P4 failed, rt=%d", rt);
        s_connecting = false;
        app_state_set_ble_status(false, false, false);
        (void)tal_sw_timer_start(s_retry_timer, T5_BLE_RESCAN_DELAY_MS, TAL_TIMER_ONCE);
        return;
    }

    T5_LOG_I("connecting BodyGuard-P4");
}

/**
 * @brief 处理扫描广播，只允许连接名称为 BodyGuard-P4 的设备。
 * @param[in] event BLE 事件。
 * @return none
 */
static void __handle_adv_report(TAL_BLE_EVT_PARAMS_T *event)
{
    TAL_BLE_ADV_REPORT_T *report;
    bodyguard_adv_cache_t *cache;
    bool name_match;
    bool service_match;

    if (event == NULL) {
        return;
    }

    report = &event->ble_event.adv_report;
    name_match = __adv_name_match(report->p_data, report->data_len);
    service_match = __adv_service_match(report->p_data, report->data_len);
    if (!name_match && !service_match) {
        return;
    }

    cache = __adv_cache_get(&report->peer_addr);
    if (cache == NULL) {
        return;
    }

    cache->name_seen = cache->name_seen || name_match;
    cache->service_seen = cache->service_seen || service_match;
    T5_LOG_I("BodyGuard-P4 candidate rssi=%d name=%u service=%u",
             report->rssi, cache->name_seen ? 1 : 0, cache->service_seen ? 1 : 0);
    if (!cache->name_seen || !cache->service_seen) {
        return;
    }

    T5_LOG_I("BodyGuard-P4 found, rssi=%d", report->rssi);
    __connect_peer(&report->peer_addr);
}

/**
 * @brief 处理连接和服务发现完成事件。
 * @param[in] event BLE 事件。
 * @return none
 */
static void __handle_connected(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }

    s_connecting = false;
    if (event->ble_event.connect.result != OPRT_OK) {
        T5_LOG_E("GATT discovery failed, rt=%d", event->ble_event.connect.result);
        app_state_set_ble_status(false, false, false);
        (void)tal_sw_timer_start(s_retry_timer, T5_BLE_RESCAN_DELAY_MS, TAL_TIMER_ONCE);
        return;
    }

    memcpy(&s_peer, &event->ble_event.connect.peer, sizeof(s_peer));
    if (s_peer.char_handle[TAL_COMMON_NOTIFY_CHAR_INDEX] == 0 ||
        s_peer.char_handle[TAL_COMMON_NOTIFY_CHAR_INDEX] == 0xFFFF) {
        T5_LOG_E("notify characteristic not found: %s", BODYGUARD_NOTIFY_CHAR_UUID_STR);
        app_state_set_ble_status(false, false, false);
        (void)tal_ble_disconnect(s_peer);
        return;
    }

    s_connected = true;
    s_retry_count = 0;
    app_state_set_ble_status(true, false, false);

    T5_LOG_I("BodyGuard-P4 connected, service=0x%04x notify=%s handle=0x%04x payload=%u",
             BODYGUARD_SERVICE_UUID, BODYGUARD_NOTIFY_CHAR_UUID_STR,
             (unsigned)s_peer.char_handle[TAL_COMMON_NOTIFY_CHAR_INDEX],
             (unsigned)BODYGUARD_PACKET_SIZE);
    (void)tal_ble_client_exchange_mtu_request(s_peer, BLE_CLIENT_MTU);
    (void)tal_sw_timer_start(s_timeout_timer, T5_BLE_TIMEOUT_MS, TAL_TIMER_ONCE);
}

/**
 * @brief 处理 notify 数据，严格校验后写入共享状态。
 * @param[in] data notify 数据。
 * @param[in] len notify 长度。
 * @return none
 */
static void __handle_notify_data(uint8_t *data, uint16_t len)
{
    ble_t5_packet_t packet;

    if (app_state_decode_packet(data, len, &packet) == false) {
        T5_LOG_E("drop invalid BLE packet, len=%u", (unsigned)len);
        return;
    }

    T5_LOG_I("BLE packet ok len=%u sys=%u risk=%u eeg=%u pose=%u tail=0x%04x",
             (unsigned)len,
             (unsigned)packet.sys_state,
             (unsigned)packet.risk_score,
             (unsigned)packet.eeg_state,
             (unsigned)packet.pose_state,
             (unsigned)packet.tail);
    (void)app_state_update_packet_ex(&packet, len >= BODYGUARD_LEARNING_V1_PACKET_SIZE);
    (void)tal_sw_timer_start(s_timeout_timer, T5_BLE_TIMEOUT_MS, TAL_TIMER_ONCE);
}

/**
 * @brief 处理断开连接，保留最后数据并重新扫描。
 * @param[in] event BLE 事件。
 * @return none
 */
static void __handle_disconnect(TAL_BLE_EVT_PARAMS_T *event)
{
    uint8_t reason = 0;

    if (event != NULL) {
        reason = event->ble_event.disconnect.reason;
    }

    T5_LOG_E("BodyGuard-P4 disconnected, reason=%u", (unsigned)reason);
    s_connected = false;
    s_connecting = false;
    s_scanning = false;
    memset(&s_peer, 0, sizeof(s_peer));
    app_state_set_ble_status(false, false, false);
    (void)tal_sw_timer_start(s_retry_timer, T5_BLE_RESCAN_DELAY_MS, TAL_TIMER_ONCE);
}

/**
 * @brief BLE 事件总回调。
 * @param[in] event BLE 事件。
 * @return none
 */
static void __ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case TAL_BLE_EVT_ADV_REPORT:
        __handle_adv_report(event);
        break;
    case TAL_BLE_EVT_CENTRAL_CONNECT_DISCOVERY:
        __handle_connected(event);
        break;
    case TAL_BLE_EVT_NOTIFY_RX:
        __handle_notify_data(event->ble_event.data_report.report.p_data, event->ble_event.data_report.report.len);
        break;
    case TAL_BLE_EVT_DISCONNECT:
        __handle_disconnect(event);
        break;
    case TAL_BLE_EVT_MTU_RSP:
        T5_LOG_I("BLE MTU=%u", (unsigned)event->ble_event.exchange_mtu.mtu);
        break;
    default:
        break;
    }
}

/**
 * @brief 重连定时器回调。
 * @param[in] timer_id 定时器 ID。
 * @param[in] arg 用户参数。
 * @return none
 */
static void __retry_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (s_connected || s_connecting || s_scanning) {
        return;
    }

    s_retry_count++;
    (void)__ble_start_scan();
}

/**
 * @brief BLE 数据超时回调，显示断线并重连。
 * @param[in] timer_id 定时器 ID。
 * @param[in] arg 用户参数。
 * @return none
 */
static void __timeout_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (s_connected) {
        T5_LOG_E("BLE data timeout, reconnect");
        (void)tal_ble_disconnect(s_peer);
    }
}

/**
 * @brief 启动 BLE 扫描。
 * @return OPRT_OK on success, error code on failure.
 */
static OPERATE_RET __ble_start_scan(void)
{
    TAL_BLE_SCAN_PARAMS_T scan_cfg;
    OPERATE_RET rt;

    rt = tal_ble_bt_init(TAL_BLE_ROLE_CENTRAL, __ble_event_cb);
    if (rt != OPRT_OK) {
        T5_LOG_E("BLE central init failed, rt=%d", rt);
        return rt;
    }

    memset(&scan_cfg, 0, sizeof(scan_cfg));
    scan_cfg.type = TAL_BLE_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_interval = BLE_SCAN_INTERVAL;
    scan_cfg.scan_window = BLE_SCAN_WINDOW;
    scan_cfg.timeout = 0x0000;
    scan_cfg.filter_dup = 0;

    rt = tal_ble_scan_start(&scan_cfg);
    if (rt != OPRT_OK) {
        T5_LOG_E("BLE scan start failed, rt=%d", rt);
        return rt;
    }

    s_ready = true;
    s_scanning = true;
    memset(s_adv_cache, 0, sizeof(s_adv_cache));
    app_state_set_ble_status(false, true, false);
    T5_LOG_I("scanning for %s", BODYGUARD_ADV_NAME);
    return OPRT_OK;
}

OPERATE_RET ble_client_init(void)
{
    (void)tal_sw_timer_create(__retry_timer_cb, NULL, &s_retry_timer);
    (void)tal_sw_timer_create(__timeout_timer_cb, NULL, &s_timeout_timer);
    return OPRT_OK;
}

void ble_client_start(void)
{
    if (s_connected || s_connecting || s_scanning) {
        return;
    }

    if (__ble_start_scan() != OPRT_OK) {
        (void)tal_sw_timer_start(s_retry_timer, T5_BLE_RESCAN_DELAY_MS, TAL_TIMER_ONCE);
    }
}

void ble_client_restart(void)
{
    if (s_connected || s_connecting) {
        (void)tal_ble_disconnect(s_peer);
    }
    if (s_scanning) {
        (void)tal_ble_scan_stop();
    }

    s_connected = false;
    s_connecting = false;
    s_scanning = false;
    app_state_set_ble_status(false, false, false);
    (void)__ble_start_scan();
}

OPERATE_RET ble_client_get_status(t5_ble_status_t *status)
{
    if (status == NULL) {
        return OPRT_INVALID_PARM;
    }

    memset(status, 0, sizeof(*status));
    status->ready = s_ready;
    status->scanning = s_scanning;
    status->connecting = s_connecting;
    status->connected = s_connected;
    status->conn_handle = s_peer.conn_handle;
    status->notify_handle = s_peer.char_handle[TAL_COMMON_NOTIFY_CHAR_INDEX];
    status->retry_count = s_retry_count;
    return OPRT_OK;
}
