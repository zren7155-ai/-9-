#include "mjpeg_stream.h"

#include <stdio.h>
#include <string.h>

#include "bodyguard_config.h"
#include "camera_ring.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_upload.h"

static const char *TAG = "mjpeg_stream";

typedef struct {
    httpd_handle_t server;
    bool initialized;
} mjpeg_stream_ctx_t;

static mjpeg_stream_ctx_t s_ctx;

static esp_err_t root_handler(httpd_req_t *req)
{
    const char html[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>BodyGuard P4 Live</title>"
        "<style>body{margin:0;background:#111;color:#eee;font-family:Arial,sans-serif}"
        "header{padding:10px 14px;background:#1d1d1d}"
        "img{display:block;width:100%;height:auto;filter:brightness(1.0) contrast(1.03) saturate(1.02);}"
        ".hint{font-size:12px;color:#bbb;margin-left:8px}</style>"
        "</head><body><header>BodyGuard-P4 OV5647 MJPEG Live Stream"
        "<span class=\"hint\">enhanced view</span></header>"
        "<img src=\"/stream\"></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
#if !BODYGUARD_MJPEG_STREAM_ENABLE
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream disabled");
    return ESP_FAIL;
#else
    uint8_t *jpg = heap_caps_malloc(BODYGUARD_MJPEG_FRAME_MAX_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpg == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_ERR_NO_MEM;
    }

    size_t jpg_len = 0;
    uint64_t ts = 0;
    esp_err_t err = camera_ring_copy_latest_jpeg(jpg, BODYGUARD_MJPEG_FRAME_MAX_BYTES,
                                                 &jpg_len, &ts);
    if (err != ESP_OK || jpg_len == 0) {
        heap_caps_free(jpg);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }

    char ts_header[32];
    snprintf(ts_header, sizeof(ts_header), "%llu", (unsigned long long)ts);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "X-BodyGuard-Timestamp", ts_header);
    err = httpd_resp_send(req, (const char *)jpg, jpg_len);
    heap_caps_free(jpg);
    return err;
#endif
}

static esp_err_t stream_handler(httpd_req_t *req)
{
#if !BODYGUARD_MJPEG_STREAM_ENABLE
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream disabled");
    return ESP_FAIL;
#else
    uint8_t *jpg = heap_caps_malloc(BODYGUARD_MJPEG_FRAME_MAX_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpg == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_ERR_NO_MEM;
    }

    char content_type[96];
    snprintf(content_type, sizeof(content_type),
             "multipart/x-mixed-replace;boundary=%s", BODYGUARD_MJPEG_BOUNDARY);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "MJPEG client connected");
    while (wifi_upload_is_connected()) {
        size_t jpg_len = 0;
        uint64_t ts = 0;
        esp_err_t err = camera_ring_copy_latest_jpeg(jpg, BODYGUARD_MJPEG_FRAME_MAX_BYTES,
                                                     &jpg_len, &ts);
        if (err != ESP_OK || jpg_len == 0) {
            ESP_LOGW(TAG, "stream capture failed: %s len=%u", esp_err_to_name(err), (unsigned)jpg_len);
            vTaskDelay(pdMS_TO_TICKS(BODYGUARD_MJPEG_FRAME_MS));
            continue;
        }

        char header[160];
        int header_len = snprintf(header, sizeof(header),
                                  "\r\n--%s\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %u\r\n"
                                  "X-BodyGuard-Timestamp: %llu\r\n\r\n",
                                  BODYGUARD_MJPEG_BOUNDARY,
                                  (unsigned)jpg_len,
                                  (unsigned long long)ts);
        if (httpd_resp_send_chunk(req, header, header_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpg, jpg_len) != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_MJPEG_FRAME_MS));
    }

    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(jpg);
    ESP_LOGI(TAG, "MJPEG client disconnected");
    return ESP_OK;
#endif
}

esp_err_t mjpeg_stream_init(void)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t mjpeg_stream_start(void)
{
#if !BODYGUARD_MJPEG_STREAM_ENABLE
    return ESP_OK;
#else
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "mjpeg stream not initialized");
    if (s_ctx.server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = BODYGUARD_MJPEG_STREAM_PORT;
    cfg.ctrl_port = BODYGUARD_MJPEG_STREAM_PORT + 1;
    cfg.stack_size = BODYGUARD_TASK_STACK_HUGE;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &cfg), TAG, "start http server failed");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t snapshot = {
        .uri = "/snapshot.jpg",
        .method = HTTP_GET,
        .handler = snapshot_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t stream = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ctx.server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ctx.server, &snapshot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_ctx.server, &stream));
    ESP_LOGI(TAG, "MJPEG stream server started: http://<p4-ip>:%d/stream",
             BODYGUARD_MJPEG_STREAM_PORT);
    return ESP_OK;
#endif
}
