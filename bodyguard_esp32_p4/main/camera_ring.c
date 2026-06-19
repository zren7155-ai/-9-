#include "camera_ring.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "bodyguard_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "camera_ring";

#define BODYGUARD_CAMERA_CAPTURE_BUF_COUNT  2
#define BODYGUARD_CAMERA_SKIP_STARTUP_FRAMES 2
#define BODYGUARD_CAMERA_JPEG_QUALITY_RING     84
#define BODYGUARD_CAMERA_JPEG_QUALITY_PREVIEW  82
#define BODYGUARD_CAMERA_JPEG_QUALITY_SNAPSHOT 90
#define BODYGUARD_CAMERA_DQBUF_TIMEOUT_MS   1200

typedef struct {
    int csi_fd;
    int jpeg_fd;
    uint8_t *csi_buffers[BODYGUARD_CAMERA_CAPTURE_BUF_COUNT];
    uint8_t *jpeg_buffer;
    size_t csi_buffer_size;
    size_t jpeg_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t csi_format;
    bool started;
} camera_pipeline_t;

typedef struct {
    camera_frame_t frames[BODYGUARD_CAMERA_RING_FRAMES];
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    size_t write_index;
    bool ready;
    bool frozen;
    bool first_frame_logged;
    uint32_t frame_period_ms;
    camera_pipeline_t pipe;
    i2c_master_bus_handle_t i2c_bus;
} camera_ring_ctx_t;

static camera_ring_ctx_t s_ctx;

static esp_err_t camera_set_jpeg_quality(uint8_t quality)
{
    struct v4l2_ext_control ctrl = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
        .value = quality,
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CID_JPEG_CLASS,
        .count = 1,
        .controls = &ctrl,
    };

    if (ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "璁剧疆 JPEG 璐ㄩ噺澶辫触锛宷uality=%u", quality);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t camera_set_dqbuf_timeout(int fd, uint32_t timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) != 0) {
        ESP_LOGW(TAG, "璁剧疆 DQBUF 瓒呮椂澶辫触 fd=%d timeout=%lu ms", fd, (unsigned long)timeout_ms);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int camera_clamp_ctrl_value(const struct v4l2_queryctrl *query, int desired)
{
    int value = desired;
    if (value < query->minimum) {
        value = query->minimum;
    }
    if (value > query->maximum) {
        value = query->maximum;
    }
    if (query->step > 1) {
        value = query->minimum + ((value - query->minimum) / query->step) * query->step;
    }
    return value;
}

static esp_err_t camera_try_set_ctrl(int fd, uint32_t id, int desired, const char *name)
{
    struct v4l2_queryctrl query = {
        .id = id,
    };
    if (ioctl(fd, VIDIOC_QUERYCTRL, &query) != 0 || (query.flags & V4L2_CTRL_FLAG_DISABLED)) {
        ESP_LOGW(TAG, "OV5647 鎺у埗椤逛笉鍙敤: %s", name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int value = camera_clamp_ctrl_value(&query, desired);
    struct v4l2_control ctrl = {
        .id = id,
        .value = value,
    };
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0) {
        ESP_LOGW(TAG, "OV5647 璁剧疆鎺у埗椤瑰け璐? %s=%d range=[%d,%d] step=%d",
                 name, value, query.minimum, query.maximum, query.step);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OV5647 鎺у埗椤瑰凡璁剧疆: %s=%d range=[%d,%d] step=%d default=%d",
             name, value, query.minimum, query.maximum, query.step, query.default_value);
    return ESP_OK;
}

static void camera_apply_stable_auto_controls(void)
{
    /*
     * 绋冲畾鑷姩鏇濆厜锛氬彧璁剧疆鑷姩鏇濆厜鐩爣鍜岃交閲忕敾璐ㄥ弬鏁般€?     * 澧炵泭鑼冨洿銆?0Hz闃查闂€佽嚜鍔ㄧ櫧骞宠　鐢?OV5647 IPA JSON 鎺у埗銆?     */
    const int fd = s_ctx.pipe.csi_fd;
    camera_try_set_ctrl(fd, V4L2_CID_CAMERA_AE_LEVEL, 115, "ae_level");
    camera_try_set_ctrl(fd, V4L2_CID_BRIGHTNESS, 0, "brightness");
    camera_try_set_ctrl(fd, V4L2_CID_CONTRAST, 8, "contrast");
    camera_try_set_ctrl(fd, V4L2_CID_EXPOSURE, 115, "exposure_target");
}

static esp_err_t camera_open_devices(void)
{
    ESP_LOGI(TAG, "Opening video devices: csi=%s jpeg=%s",
             ESP_VIDEO_MIPI_CSI_DEVICE_NAME, ESP_VIDEO_JPEG_DEVICE_NAME);
    s_ctx.pipe.csi_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_ctx.pipe.csi_fd >= 0, ESP_FAIL, TAG, "鎵撳紑 CSI 璁惧澶辫触");

    s_ctx.pipe.jpeg_fd = open(ESP_VIDEO_JPEG_DEVICE_NAME, O_RDONLY);
    if (s_ctx.pipe.jpeg_fd < 0) {
        close(s_ctx.pipe.csi_fd);
        s_ctx.pipe.csi_fd = -1;
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "鎵撳紑 JPEG 璁惧澶辫触");
    }

    camera_set_dqbuf_timeout(s_ctx.pipe.csi_fd, BODYGUARD_CAMERA_DQBUF_TIMEOUT_MS);
    camera_set_dqbuf_timeout(s_ctx.pipe.jpeg_fd, BODYGUARD_CAMERA_DQBUF_TIMEOUT_MS);

    return ESP_OK;
}

static esp_err_t camera_query_default_format(uint32_t *width, uint32_t *height)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "璇诲彇 CSI 榛樿鏍煎紡澶辫触");

    *width = fmt.fmt.pix.width;
    *height = fmt.fmt.pix.height;
    return ESP_OK;
}

static esp_err_t camera_select_capture_format(uint32_t *capture_fmt)
{
    static const uint32_t candidates[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_UYVY,
        V4L2_PIX_FMT_RGB24,
        V4L2_PIX_FMT_GREY,
    };

    for (uint32_t fmt_index = 0; ; fmt_index++) {
        struct v4l2_fmtdesc desc = {
            .index = fmt_index,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };
        if (ioctl(s_ctx.pipe.csi_fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            break;
        }
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (desc.pixelformat == candidates[i]) {
                *capture_fmt = desc.pixelformat;
                return ESP_OK;
            }
        }
    }

    ESP_LOGE(TAG, "OV5647 褰撳墠杈撳嚭鏍煎紡涓嶆敮鎸?JPEG 纭紪閾捐矾");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t camera_configure_csi(uint32_t width, uint32_t height, uint32_t capture_fmt)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = width,
        .fmt.pix.height = height,
        .fmt.pix.pixelformat = capture_fmt,
    };
    struct v4l2_requestbuffers req = {
        .count = BODYGUARD_CAMERA_CAPTURE_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_S_FMT, &fmt) == 0, ESP_FAIL, TAG, "閰嶇疆 CSI 杈撳嚭澶辫触");
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "鐢宠 CSI 缂撳啿澶辫触");

    for (uint32_t i = 0; i < BODYGUARD_CAMERA_CAPTURE_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "鏌ヨ CSI 缂撳啿澶辫触");

        s_ctx.pipe.csi_buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_ctx.pipe.csi_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_ctx.pipe.csi_buffers[i] != MAP_FAILED, ESP_ERR_NO_MEM, TAG, "鏄犲皠 CSI 缂撳啿澶辫触");

        s_ctx.pipe.csi_buffer_size = buf.length;
        ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "鍥炲～ CSI 缂撳啿澶辫触");
    }

    return ESP_OK;
}

static esp_err_t camera_configure_jpeg(uint32_t width, uint32_t height, uint32_t capture_fmt)
{
    struct v4l2_format out_fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix.width = width,
        .fmt.pix.height = height,
        .fmt.pix.pixelformat = capture_fmt,
    };
    struct v4l2_requestbuffers out_req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    struct v4l2_format cap_fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = width,
        .fmt.pix.height = height,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG,
    };
    struct v4l2_requestbuffers cap_req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct v4l2_buffer cap_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = 0,
    };

    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_S_FMT, &out_fmt) == 0, ESP_FAIL, TAG, "閰嶇疆 JPEG 杈撳叆澶辫触");
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_REQBUFS, &out_req) == 0, ESP_FAIL, TAG, "鐢宠 JPEG 杈撳嚭缂撳啿澶辫触");
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_S_FMT, &cap_fmt) == 0, ESP_FAIL, TAG, "閰嶇疆 JPEG 鎹曡幏澶辫触");
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_REQBUFS, &cap_req) == 0, ESP_FAIL, TAG, "鐢宠 JPEG 鎹曡幏缂撳啿澶辫触");
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QUERYBUF, &cap_buf) == 0, ESP_FAIL, TAG, "鏌ヨ JPEG 缂撳啿澶辫触");

    s_ctx.pipe.jpeg_buffer = mmap(NULL, cap_buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s_ctx.pipe.jpeg_fd, cap_buf.m.offset);
    ESP_RETURN_ON_FALSE(s_ctx.pipe.jpeg_buffer != MAP_FAILED, ESP_ERR_NO_MEM, TAG, "鏄犲皠 JPEG 缂撳啿澶辫触");
    s_ctx.pipe.jpeg_buffer_size = cap_buf.length;
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QBUF, &cap_buf) == 0, ESP_FAIL, TAG, "鍥炲～ JPEG 缂撳啿澶辫触");
    ESP_RETURN_ON_ERROR(camera_set_jpeg_quality(BODYGUARD_CAMERA_JPEG_QUALITY_RING), TAG, "璁剧疆榛樿 JPEG 璐ㄩ噺澶辫触");

    return ESP_OK;
}

static esp_err_t camera_start_pipeline(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "鍚姩 JPEG 鎹曡幏澶辫触");

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "鍚姩 JPEG 杈撳嚭澶辫触");

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "鍚姩 CSI 閲囬泦澶辫触");

    for (int i = 0; i < BODYGUARD_CAMERA_SKIP_STARTUP_FRAMES; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_ctx.pipe.csi_fd, VIDIOC_DQBUF, &buf) == 0) {
            ioctl(s_ctx.pipe.csi_fd, VIDIOC_QBUF, &buf);
        }
    }

    s_ctx.pipe.started = true;
    return ESP_OK;
}

static esp_err_t camera_hw_init(void)
{
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = s_ctx.i2c_bus,
            .freq = BODYGUARD_I2C_PORT_FREQ_HZ,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = false,
    };
    esp_video_init_config_t video_cfg = {
        .csi = &csi_cfg,
        .jpeg = NULL,
    };
    const uint32_t init_flags = ESP_VIDEO_INIT_FLAGS_MIPI_CSI |
                                ESP_VIDEO_INIT_FLAGS_ISP |
                                ESP_VIDEO_INIT_FLAGS_JPEG;

    ESP_LOGI(TAG, "Initializing esp_video with MIPI CSI + ISP + JPEG");
    ESP_RETURN_ON_ERROR(esp_video_init_with_flags(&video_cfg, init_flags), TAG, "init esp_video failed");
    ESP_LOGI(TAG, "esp_video init done");

    ESP_LOGI(TAG, "Opening OV5647 video nodes");
    ESP_RETURN_ON_ERROR(camera_open_devices(), TAG, "open camera devices failed");
    ESP_LOGI(TAG, "Video nodes opened");

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t capture_fmt = 0;
    ESP_LOGI(TAG, "Querying OV5647 default V4L2 format");
    ESP_RETURN_ON_ERROR(camera_query_default_format(&width, &height), TAG, "query default video format failed");
    ESP_LOGI(TAG, "Selecting JPEG input pixel format");
    ESP_RETURN_ON_ERROR(camera_select_capture_format(&capture_fmt), TAG, "select JPEG input pixel format failed");
    ESP_LOGI(TAG, "Configuring CSI capture path");
    ESP_RETURN_ON_ERROR(camera_configure_csi(width, height, capture_fmt), TAG, "configure CSI path failed");
    ESP_LOGI(TAG, "Configuring JPEG encode path");
    ESP_RETURN_ON_ERROR(camera_configure_jpeg(width, height, capture_fmt), TAG, "configure JPEG path failed");
    ESP_LOGI(TAG, "Starting OV5647 CSI + JPEG streams");
    ESP_RETURN_ON_ERROR(camera_start_pipeline(), TAG, "start camera stream failed");
    camera_apply_stable_auto_controls();

    s_ctx.pipe.width = width;
    s_ctx.pipe.height = height;
    s_ctx.pipe.csi_format = capture_fmt;

    ESP_LOGI(TAG, "OV5647 camera pipeline ready: %lux%lu format=%08lx",
             (unsigned long)width, (unsigned long)height, (unsigned long)capture_fmt);
    return ESP_OK;
}

static esp_err_t camera_capture_jpeg(uint8_t *dst, size_t cap, size_t *out_size, uint8_t quality)
{
    esp_err_t ret = ESP_OK;
    struct v4l2_buffer csi_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct v4l2_buffer jpeg_out = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    struct v4l2_buffer jpeg_cap = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    ESP_RETURN_ON_FALSE(dst && out_size, ESP_ERR_INVALID_ARG, TAG, "capture args are null");
    ESP_RETURN_ON_FALSE(s_ctx.pipe.started, ESP_ERR_INVALID_STATE, TAG, "camera pipeline not started");
    ESP_RETURN_ON_ERROR(camera_set_jpeg_quality(quality),
                        TAG, "set JPEG quality failed");

    ESP_RETURN_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_DQBUF, &csi_buf) == 0, ESP_FAIL, TAG, "dequeue CSI frame failed");

    jpeg_out.m.userptr = (unsigned long)s_ctx.pipe.csi_buffers[csi_buf.index];
    jpeg_out.length = csi_buf.bytesused;
    ESP_GOTO_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QBUF, &jpeg_out) == 0, ESP_FAIL, fail0, TAG, "鎻愪氦 JPEG 杈撳叆澶辫触");
    ESP_GOTO_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_DQBUF, &jpeg_cap) == 0, ESP_FAIL, fail0, TAG, "鑾峰彇 JPEG 杈撳嚭澶辫触");

    if (jpeg_cap.bytesused > cap) {
        ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QBUF, &jpeg_cap);
        ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_SIZE, fail0, TAG, "JPEG 缂撳啿绌洪棿涓嶈冻");
    }

    memcpy(dst, s_ctx.pipe.jpeg_buffer, jpeg_cap.bytesused);
    *out_size = jpeg_cap.bytesused;

    ESP_GOTO_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QBUF, &jpeg_cap) == 0, ESP_FAIL, fail0, TAG, "鍥炲～ JPEG 杈撳嚭缂撳啿澶辫触");
    ESP_GOTO_ON_FALSE(ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_DQBUF, &jpeg_out) == 0, ESP_FAIL, fail0, TAG, "鍥炴敹 JPEG 杈撳叆缂撳啿澶辫触");
    ESP_GOTO_ON_FALSE(ioctl(s_ctx.pipe.csi_fd, VIDIOC_QBUF, &csi_buf) == 0, ESP_FAIL, fail1, TAG, "鍥炲～ CSI 缂撳啿澶辫触");

    return ESP_OK;

fail1:
    ioctl(s_ctx.pipe.jpeg_fd, VIDIOC_QBUF, &jpeg_cap);
fail0:
    ioctl(s_ctx.pipe.csi_fd, VIDIOC_QBUF, &csi_buf);
    return ret;
}

static esp_err_t allocate_frame_slots(void)
{
    for (size_t i = 0; i < BODYGUARD_CAMERA_RING_FRAMES; i++) {
        s_ctx.frames[i].data = heap_caps_malloc(BODYGUARD_CAMERA_JPEG_SLOT_SIZE,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ctx.frames[i].data == NULL) {
        ESP_LOGE(TAG, "PSRAM JPEG slot alloc failed index=%u", (unsigned)i);
            return ESP_ERR_NO_MEM;
        }
        s_ctx.frames[i].size = 0;
        s_ctx.frames[i].valid = false;
    }
    return ESP_OK;
}

esp_err_t camera_ring_init_with_i2c(i2c_master_bus_handle_t i2c_bus)
{
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_ARG, TAG, "camera I2C bus is null");
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.pipe.csi_fd = -1;
    s_ctx.pipe.jpeg_fd = -1;
    s_ctx.i2c_bus = i2c_bus;

    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(allocate_frame_slots(), TAG, "allocate PSRAM ring frames failed");
    ESP_RETURN_ON_ERROR(camera_hw_init(), TAG, "init OV5647 camera failed");

    s_ctx.frame_period_ms = 1000 / BODYGUARD_CAMERA_LOW_FPS;
    s_ctx.ready = true;
    ESP_LOGI(TAG, "camera ring buffer ready: frames=%d slot_size=%d bytes",
             BODYGUARD_CAMERA_RING_FRAMES, BODYGUARD_CAMERA_JPEG_SLOT_SIZE);
    return ESP_OK;
}

esp_err_t camera_ring_init(void)
{
    ESP_LOGE(TAG, "ESP32-P4 NANO camera and MPU6050 share I2C GPIO7/8; call camera_ring_init_with_i2c()");
    return ESP_ERR_INVALID_STATE;
}

static void camera_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "task_camera started, continuously writing OV5647 JPEG frames to ring buffer");

    while (true) {
        if (!s_ctx.ready) {
            vTaskDelay(pdMS_TO_TICKS(BODYGUARD_CAMERA_RETRY_MS));
            continue;
        }

        if (!s_ctx.frozen && xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            camera_frame_t *slot = &s_ctx.frames[s_ctx.write_index];
            size_t size = 0;
            esp_err_t err = camera_capture_jpeg(slot->data, BODYGUARD_CAMERA_JPEG_SLOT_SIZE, &size,
                                                BODYGUARD_CAMERA_JPEG_QUALITY_RING);
            if (err == ESP_OK) {
                slot->size = size;
                slot->timestamp_ms = bodyguard_now_ms();
                slot->valid = true;
                if (!s_ctx.first_frame_logged) {
                    s_ctx.first_frame_logged = true;
                    ESP_LOGI(TAG, "OV5647 棣栧抚 JPEG 閲囬泦鎴愬姛: %u 瀛楄妭", (unsigned)size);
                }
                s_ctx.write_index = (s_ctx.write_index + 1) % BODYGUARD_CAMERA_RING_FRAMES;
            } else {
                ESP_LOGW(TAG, "JPEG 閲囬泦澶辫触: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_ctx.lock);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(s_ctx.frame_period_ms));
    }
}

esp_err_t camera_ring_start(void)
{
    if (!s_ctx.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(camera_task, "task_camera", BODYGUARD_TASK_STACK_LARGE, NULL,
                                BODYGUARD_TASK_PRIO_CAMERA, &s_ctx.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t camera_ring_export_event_window(event_window_t *window)
{
    const size_t max_event_frames = BODYGUARD_EVENT_CAPTURE_MAX_IMAGES;

    if (window == NULL || !s_ctx.ready) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(window, 0, sizeof(*window));
    s_ctx.frozen = true;
    vTaskDelay(pdMS_TO_TICKS(5));

    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_ctx.frozen = false;
        return ESP_ERR_TIMEOUT;
    }

    for (size_t n = 0; n < BODYGUARD_CAMERA_RING_FRAMES &&
                       window->image_count < max_event_frames; n++) {
        size_t idx = (s_ctx.write_index + n) % BODYGUARD_CAMERA_RING_FRAMES;
        if (s_ctx.frames[idx].valid) {
            window->images[window->image_count++] = s_ctx.frames[idx];
        }
    }
    xSemaphoreGive(s_ctx.lock);

    if (window->image_count > 0) {
        window->snapshot = window->images[window->image_count - 1];
    }
    if (window->image_count > 0) {
        ESP_LOGI(TAG, "event window exported from ring frames=%u snapshot=%u",
                 (unsigned)window->image_count, window->snapshot.valid ? 1 : 0);
        s_ctx.frozen = false;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "event ring has no valid frame, capture fallback frames");

    /*
     * Fallback capture: if the ring buffer has no valid frames yet, capture a
     * few event frames here so the cloud AI still receives real images.
     */
    while (window->image_count < BODYGUARD_EVENT_MIN_VIDEO_FRAMES &&
           window->image_count < max_event_frames &&
           window->image_count < BODYGUARD_CAMERA_RING_FRAMES) {
        camera_frame_t *frame = &window->images[window->image_count];
        frame->data = heap_caps_malloc(BODYGUARD_CAMERA_JPEG_SLOT_SIZE,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (frame->data == NULL) {
            ESP_LOGW(TAG, "event fallback frame PSRAM alloc failed");
            break;
        }

        size_t size = 0;
        esp_err_t cap_err = camera_capture_jpeg(frame->data,
                                                BODYGUARD_CAMERA_JPEG_SLOT_SIZE,
                                                &size,
                                                BODYGUARD_CAMERA_JPEG_QUALITY_RING);
        if (cap_err != ESP_OK || size < 1024) {
            ESP_LOGW(TAG, "event fallback frame capture failed: %s size=%u",
                     esp_err_to_name(cap_err), (unsigned)size);
            heap_caps_free(frame->data);
            memset(frame, 0, sizeof(*frame));
            break;
        }

        frame->size = size;
        frame->timestamp_ms = bodyguard_now_ms();
        frame->valid = true;
        frame->owned = true;
        window->image_count++;
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    window->snapshot.data = heap_caps_malloc(BODYGUARD_CAMERA_SNAPSHOT_MAX_SIZE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (window->snapshot.data != NULL) {
        size_t size = 0;
        if (camera_capture_jpeg(window->snapshot.data, BODYGUARD_CAMERA_SNAPSHOT_MAX_SIZE, &size,
                                BODYGUARD_CAMERA_JPEG_QUALITY_SNAPSHOT) == ESP_OK) {
            window->snapshot.size = size;
            window->snapshot.timestamp_ms = bodyguard_now_ms();
            window->snapshot.valid = true;
            window->snapshot.owned = true;
        } else {
            heap_caps_free(window->snapshot.data);
            window->snapshot.data = NULL;
            ESP_LOGW(TAG, "snapshot capture failed");
        }
    } else {
        ESP_LOGW(TAG, "snapshot PSRAM alloc failed");
    }

    s_ctx.frozen = false;
    return ESP_OK;
}

bool camera_ring_is_ready(void)
{
    return s_ctx.ready;
}

esp_err_t camera_ring_copy_latest_jpeg(uint8_t *dst, size_t cap, size_t *out_size, uint64_t *timestamp_ms)
{
    if (dst == NULL || out_size == NULL || cap == 0 || !s_ctx.ready) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = 0;
    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (size_t n = 0; n < BODYGUARD_CAMERA_RING_FRAMES; n++) {
        size_t back = n + 1;
        size_t idx = (s_ctx.write_index + BODYGUARD_CAMERA_RING_FRAMES - back) %
                     BODYGUARD_CAMERA_RING_FRAMES;
        camera_frame_t *frame = &s_ctx.frames[idx];
        if (!frame->valid || frame->data == NULL || frame->size == 0) {
            continue;
        }
        if (frame->size > cap) {
            ret = ESP_ERR_INVALID_SIZE;
            continue;
        }
        memcpy(dst, frame->data, frame->size);
        *out_size = frame->size;
        if (timestamp_ms != NULL) {
            *timestamp_ms = frame->timestamp_ms;
        }
        ret = ESP_OK;
        break;
    }

    xSemaphoreGive(s_ctx.lock);
    return ret;
}

esp_err_t camera_ring_capture_preview_jpeg(uint8_t *dst, size_t cap, size_t *out_size, uint64_t *timestamp_ms)
{
    if (dst == NULL || out_size == NULL || cap == 0 || !s_ctx.ready) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_ctx.lock, pdMS_TO_TICKS(80)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = camera_capture_jpeg(dst, cap, out_size, BODYGUARD_CAMERA_JPEG_QUALITY_PREVIEW);
    if (ret == ESP_OK && timestamp_ms != NULL) {
        *timestamp_ms = bodyguard_now_ms();
    }

    xSemaphoreGive(s_ctx.lock);
    return ret;
}
