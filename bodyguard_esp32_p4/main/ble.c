#include "ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bodyguard_config.h"
#include "data_def.h"
#include "eeg.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "adaptive_learning.h"
#include "risk_engine.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "state_machine.h"
#include "wifi_upload.h"

static const char *TAG = "ble_t5";

static const ble_uuid16_t s_t5_service_uuid = BLE_UUID16_INIT(0xFD50);
static const ble_uuid128_t s_t5_packet_uuid =
    BLE_UUID128_INIT(0xd0, 0x07, 0x9b, 0x5f, 0x80, 0x00, 0x01, 0x80,
                     0x01, 0x10, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00);

typedef struct {
    TaskHandle_t task_handle;
    bool initialized;
    bool ready;
    bool notify_enabled;
    uint16_t peripheral_conn_handle;
    uint16_t packet_val_handle;
    uint8_t own_addr_type;
    ble_t5_packet_t latest_packet;
    portMUX_TYPE lock;
} ble_ctx_t;

static ble_ctx_t s_ctx = {
    .peripheral_conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

void ble_store_config_init(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int ble_packet_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_t5_service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]) {
                {
                    .uuid = &s_t5_packet_uuid.u,
                    .access_cb = ble_packet_access,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_ctx.packet_val_handle,
                },
                {0},
            },
    },
    {0},
};

static void fill_t5_packet(ble_t5_packet_t *packet)
{
    risk_sample_t risk = risk_engine_get_latest();
    eeg_sample_t eeg = eeg_get_latest();
    system_state_t state = state_machine_get_state();
    adaptive_params_t learning = adaptive_learning_get_params();

    memset(packet, 0, sizeof(*packet));
    packet->head = BLE_HEAD;
    packet->sys_state = risk.sys_state;
    if (packet->sys_state == 0) {
        packet->sys_state = (state == STATE_ALERT) ? 3 : 1;
    }
    packet->risk_score = risk.risk_final;
    packet->eeg_state = risk.eeg_state;
    packet->pose_state = risk.pose_state;
    packet->ble_conn_state = (s_ctx.peripheral_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? 1 : 0;
    packet->cloud_conn_state = wifi_upload_is_connected() ? 1 : 0;
    strlcpy(packet->event_id, event_queue_current_event_id(), sizeof(packet->event_id));
    strlcpy(packet->ai_report,
            risk.ai_report[0] ? risk.ai_report : "等待AI分析...",
            sizeof(packet->ai_report));
    if (!risk.ai_report[0]) {
        strlcpy(packet->ai_report, "等待AI分析...", sizeof(packet->ai_report));
    }
    packet->posture_weight = learning.pose_weight;
    packet->eeg_weight = learning.eeg_weight;
    packet->burst_weight = learning.burst_weight;
    packet->warning_threshold = learning.warning_threshold;
    packet->danger_threshold = learning.danger_threshold;
    packet->false_alarm_count = learning.false_alarm_count;
    packet->confirmed_danger_count = learning.confirmed_danger_count;
    packet->missed_danger_count = learning.missed_danger_count;
    packet->learning_update_count = learning.update_count;
    packet->learning_enabled = 1;
    packet->eeg_attention = eeg.attention;
    packet->eeg_meditation = eeg.relax;
    packet->eeg_fatigue = eeg.fatigue;
    packet->eeg_signal_quality = eeg.signal_quality;
    packet->eeg_connected = eeg.connected ? 1 : 0;
    packet->eeg_score = risk.eeg_score;
    packet->tail = BLE_TAIL;
}

static int ble_packet_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ble_t5_packet_t snapshot;
    taskENTER_CRITICAL(&s_ctx.lock);
    snapshot = s_ctx.latest_packet;
    taskEXIT_CRITICAL(&s_ctx.lock);

    int rc = os_mbuf_append(ctxt->om, &snapshot, sizeof(snapshot));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    if (!s_ctx.ready || ble_gap_adv_active()) {
        return;
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BODYGUARD_BLE_DEVICE_NAME;
    fields.name_len = strlen(BODYGUARD_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t *)&s_t5_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_ctx.own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "start advertising failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "T5 BLE advertising started name=%s", BODYGUARD_BLE_DEVICE_NAME);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            taskENTER_CRITICAL(&s_ctx.lock);
            s_ctx.peripheral_conn_handle = event->connect.conn_handle;
            s_ctx.notify_enabled = false;
            taskEXIT_CRITICAL(&s_ctx.lock);
            ESP_LOGI(TAG, "T5 connected conn_handle=%u", event->connect.conn_handle);
        } else {
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        taskENTER_CRITICAL(&s_ctx.lock);
        s_ctx.peripheral_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ctx.notify_enabled = false;
        taskEXIT_CRITICAL(&s_ctx.lock);
        ESP_LOGW(TAG, "T5 disconnected reason=%d", event->disconnect.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ctx.packet_val_handle) {
            taskENTER_CRITICAL(&s_ctx.lock);
            s_ctx.notify_enabled = event->subscribe.cur_notify != 0;
            taskEXIT_CRITICAL(&s_ctx.lock);
            ESP_LOGI(TAG, "T5 notify=%d", event->subscribe.cur_notify != 0);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        return 0;

    default:
        return 0;
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_ctx.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    s_ctx.ready = true;
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    s_ctx.ready = false;
    s_ctx.notify_enabled = false;
    s_ctx.peripheral_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
}

static esp_err_t bodyguard_ble_transport_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_services);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(BODYGUARD_BLE_DEVICE_NAME);
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static void ble_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "ble_t5_task started period=%dms", BODYGUARD_BLE_PERIOD_MS);

    while (true) {
        ble_t5_packet_t packet;
        fill_t5_packet(&packet);

        taskENTER_CRITICAL(&s_ctx.lock);
        s_ctx.latest_packet = packet;
        bool can_notify = s_ctx.ready && s_ctx.notify_enabled &&
                          s_ctx.peripheral_conn_handle != BLE_HS_CONN_HANDLE_NONE;
        uint16_t conn_handle = s_ctx.peripheral_conn_handle;
        uint16_t packet_handle = s_ctx.packet_val_handle;
        taskEXIT_CRITICAL(&s_ctx.lock);

        if (can_notify) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&packet, sizeof(packet));
            if (om != NULL) {
                int rc = ble_gatts_notify_custom(conn_handle, packet_handle, om);
                if (rc != 0) {
                    ESP_LOGW(TAG, "T5 notify failed rc=%d", rc);
                }
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_BLE_PERIOD_MS));
    }
}

esp_err_t ble_init(void)
{
    memset(&s_ctx.latest_packet, 0, sizeof(s_ctx.latest_packet));
    s_ctx.initialized = true;
    s_ctx.ready = false;
    s_ctx.notify_enabled = false;
    s_ctx.peripheral_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    ESP_LOGI(TAG, "T5 BLE init name=%s service=0xFD50", BODYGUARD_BLE_DEVICE_NAME);
    return bodyguard_ble_transport_init();
}

esp_err_t ble_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "ble not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(ble_task, "ble_t5_task", BODYGUARD_TASK_STACK_MEDIUM, NULL,
                                BODYGUARD_TASK_PRIO_BLE, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool ble_is_ready(void)
{
    return s_ctx.ready;
}
