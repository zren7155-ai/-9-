#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define TAG "brainlink_gateway"

#define REMOTE_DEVICE_NAME       "BrainLink_Lite"
#define BRAINLINK_PIN_CODE       "1234"
#define UART_FORWARD_PORT        UART_NUM_1
#define UART_FORWARD_TX_GPIO     GPIO_NUM_17
#define UART_FORWARD_RX_GPIO     GPIO_NUM_16
#define UART_FORWARD_BAUD        115200

#define EEG_FRAME_HEAD           0xBB
#define EEG_FRAME_TAIL           0xEE

static const esp_spp_mode_t s_spp_mode = ESP_SPP_MODE_CB;
static const bool s_spp_enable_l2cap_ertm = true;
static const esp_spp_sec_t s_sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t s_role_master = ESP_SPP_ROLE_MASTER;
static const esp_bt_inq_mode_t s_inq_mode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
static const uint8_t s_inq_len = 10;
static const uint8_t s_inq_num_rsps = 0;

static esp_bd_addr_t s_peer_bd_addr = {0};
static uint32_t s_spp_handle = 0;
static bool s_discovery_started = false;
static uint8_t s_latest_attention = 0;
static uint8_t s_latest_meditation = 0;
static uint8_t s_latest_signal = 200;

static char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, char *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (eir == NULL) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (rmt_bdname == NULL) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }
    if (rmt_bdname == NULL) {
        return false;
    }

    if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
        rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
    }
    memcpy(bdname, rmt_bdname, rmt_bdname_len);
    bdname[rmt_bdname_len] = '\0';
    *bdname_len = rmt_bdname_len;
    return true;
}

static inline uint8_t meditation_to_fatigue(uint8_t meditation)
{
    return meditation >= 100 ? 0 : (uint8_t)(100 - meditation);
}

static void forward_latest_eeg(void)
{
    uint8_t frame[5] = {
        EEG_FRAME_HEAD,
        s_latest_attention,
        meditation_to_fatigue(s_latest_meditation),
        s_latest_signal,
        EEG_FRAME_TAIL,
    };
    uart_write_bytes(UART_FORWARD_PORT, (const char *)frame, sizeof(frame));
}

static uint8_t tg_checksum(const uint8_t *payload, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += payload[i];
    }
    return (uint8_t)((~sum) & 0xFF);
}

static void parse_thinkgear_payload(const uint8_t *payload, size_t len)
{
    size_t i = 0;
    bool changed = false;

    while (i < len) {
        uint8_t code = payload[i++];
        const uint8_t *value = NULL;
        size_t value_len = 0;

        if (code >= 0x80) {
            if (i >= len) {
                break;
            }
            value_len = payload[i++];
            if (i + value_len > len) {
                break;
            }
            value = &payload[i];
            i += value_len;
        } else {
            if (i >= len) {
                break;
            }
            value = &payload[i++];
            value_len = 1;
        }

        if (code == 0x02 && value_len >= 1) {
            s_latest_signal = value[0];
            changed = true;
        } else if (code == 0x04 && value_len >= 1) {
            s_latest_attention = value[0];
            changed = true;
        } else if (code == 0x05 && value_len >= 1) {
            s_latest_meditation = value[0];
            changed = true;
        }
    }

    if (changed) {
        forward_latest_eeg();
        ESP_LOGI(TAG, "BrainLink EEG attention=%u meditation=%u signal=%u",
                 s_latest_attention, s_latest_meditation, s_latest_signal);
    }
}

static void handle_spp_stream(const uint8_t *data, size_t len)
{
    static uint8_t buf[256];
    static size_t used = 0;

    for (size_t i = 0; i < len; i++) {
        if (used < sizeof(buf)) {
            buf[used++] = data[i];
        } else {
            used = 0;
        }
    }

    while (used >= 4) {
        size_t start = SIZE_MAX;
        for (size_t i = 0; i + 1 < used; i++) {
            if (buf[i] == 0xAA && buf[i + 1] == 0xAA) {
                start = i;
                break;
            }
        }
        if (start == SIZE_MAX) {
            used = 0;
            return;
        }
        if (start > 0) {
            memmove(buf, buf + start, used - start);
            used -= start;
        }
        if (used < 4) {
            return;
        }

        uint8_t payload_len = buf[2];
        size_t frame_len = (size_t)payload_len + 4;
        if (used < frame_len) {
            return;
        }

        const uint8_t *payload = &buf[3];
        uint8_t checksum = buf[3 + payload_len];
        if (tg_checksum(payload, payload_len) == checksum) {
            parse_thinkgear_payload(payload, payload_len);
        }

        memmove(buf, buf + frame_len, used - frame_len);
        used -= frame_len;
    }
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
        uint8_t name_len = 0;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR &&
                get_name_from_eir(param->disc_res.prop[i].val, name, &name_len)) {
                if (strlen(REMOTE_DEVICE_NAME) == name_len &&
                    strncmp(name, REMOTE_DEVICE_NAME, name_len) == 0) {
                    memcpy(s_peer_bd_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    ESP_LOGI(TAG, "Found BrainLink by name: %s", REMOTE_DEVICE_NAME);
                    esp_bt_gap_cancel_discovery();
                    esp_spp_start_discovery(s_peer_bd_addr);
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {0};
        memcpy(pin_code, BRAINLINK_PIN_CODE, 4);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        ESP_LOGI(TAG, "PIN reply sent");
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && !s_discovery_started) {
            s_discovery_started = true;
        }
        break;
    default:
        break;
    }
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "SPP init complete");
        esp_bt_gap_set_device_name("BrainLink-Gateway");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        esp_bt_gap_start_discovery(s_inq_mode, s_inq_len, s_inq_num_rsps);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
            ESP_LOGI(TAG, "SPP service found, connecting");
            esp_spp_connect(s_sec_mask, s_role_master, param->disc_comp.scn[0], s_peer_bd_addr);
        } else {
            ESP_LOGW(TAG, "SPP service discovery failed: %d", param->disc_comp.status);
            esp_bt_gap_start_discovery(s_inq_mode, s_inq_len, s_inq_num_rsps);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        if (param->open.status == ESP_SPP_SUCCESS) {
            s_spp_handle = param->open.handle;
            ESP_LOGI(TAG, "SPP opened handle=%" PRIu32 " peer=%s",
                     s_spp_handle, bda2str(param->open.rem_bda, bda_str, sizeof(bda_str)));
        } else {
            ESP_LOGW(TAG, "SPP open failed: %d", param->open.status);
        }
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGW(TAG, "SPP closed, restarting discovery");
        s_spp_handle = 0;
        esp_bt_gap_start_discovery(s_inq_mode, s_inq_len, s_inq_num_rsps);
        break;
    case ESP_SPP_DATA_IND_EVT:
        handle_spp_stream(param->data_ind.data, param->data_ind.len);
        break;
    default:
        break;
    }
}

static void uart_init_forward(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_FORWARD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_FORWARD_PORT, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_FORWARD_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_FORWARD_PORT, UART_FORWARD_TX_GPIO, UART_FORWARD_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    esp_err_t ret;
    char bda_str[18] = {0};

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uart_init_forward();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_cfg.ssp_en = false;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(spp_cb));

    esp_spp_cfg_t bt_spp_cfg = {
        .mode = s_spp_mode,
        .enable_l2cap_ertm = s_spp_enable_l2cap_ertm,
        .tx_buffer_size = 0,
    };
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&bt_spp_cfg));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(TAG, "Gateway ready, local BT=%s, UART TX=%d RX=%d",
             bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)),
             UART_FORWARD_TX_GPIO, UART_FORWARD_RX_GPIO);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
