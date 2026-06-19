#include "sensor.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/task.h"

#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_SMPLRT_DIV    0x19
#define MPU6050_REG_CONFIG        0x1A
#define MPU6050_REG_GYRO_CONFIG   0x1B
#define MPU6050_REG_ACCEL_CONFIG  0x1C
#define MPU6050_REG_SIGNAL_RESET  0x68
#define MPU6050_REG_WHO_AM_I      0x75
#define MPU6050_REG_PWR_MGMT_2    0x6C
#define MPU6050_REG_DATA_START    0x3B
#define MPU6050_WHO_AM_I_VALUE    0x68
#define MPU6050_ADDR_ALT          0x69
#define MPU6050_WAKE_CHECK_MS     1000
#define MPU6050_ZERO_RECOVER_COUNT 100
#define MPU6050_READ_RECOVER_COUNT 20
#define MPU6050_CALIBRATION_SAMPLES 80
#define MPU6050_CALIBRATION_MAX_GYRO_DPS 8.0f
#define MPU6050_CALIBRATION_MIN_ACCEL_G 0.75f
#define MPU6050_CALIBRATION_MAX_ACCEL_G 1.35f

static const char *TAG = "sensor";

typedef struct {
    QueueHandle_t output_queue;
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t mpu_handle;
    TaskHandle_t task_handle;
    sensor_sample_t latest;
    float last_tilt_angle;
    uint64_t next_wake_check_ms;
    uint8_t read_fail_count;
    uint8_t zero_data_count;
    float accel_lsb_per_g;
    float gyro_lsb_per_dps;
    float accel_ref[3];
    float gyro_bias[3];
    bool calibrated;
    uint16_t calibration_count;
    bool force_wake_check;
    bool initialized;
} sensor_ctx_t;

static sensor_ctx_t s_ctx;

static uint8_t mpu_read_reg_or_ff(uint8_t reg);

static esp_err_t mpu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_ctx.mpu_handle, payload, sizeof(payload), -1);
}

static esp_err_t mpu_read_regs(uint8_t reg, uint8_t *buffer, size_t len)
{
    return i2c_master_transmit_receive(s_ctx.mpu_handle, &reg, 1, buffer, len, -1);
}

static void mpu_dump_regs(const char *phase)
{
    uint8_t who = mpu_read_reg_or_ff(MPU6050_REG_WHO_AM_I);
    uint8_t pwr1 = mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_1);
    uint8_t pwr2 = mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_2);
    uint8_t smplrt = mpu_read_reg_or_ff(MPU6050_REG_SMPLRT_DIV);
    uint8_t cfg = mpu_read_reg_or_ff(MPU6050_REG_CONFIG);
    uint8_t gyro = mpu_read_reg_or_ff(MPU6050_REG_GYRO_CONFIG);
    uint8_t accel = mpu_read_reg_or_ff(MPU6050_REG_ACCEL_CONFIG);

    ESP_LOGW(TAG,
             "MPU6050 DIAG %s who=0x%02X pwr1=0x%02X pwr2=0x%02X smplrt=0x%02X cfg=0x%02X gyro=0x%02X accel=0x%02X",
             phase != NULL ? phase : "unknown", who, pwr1, pwr2, smplrt, cfg, gyro, accel);
}

static void mpu_probe_known_addresses(void)
{
    static const uint8_t addrs[] = {
        BODYGUARD_MPU6050_ADDR,
        MPU6050_ADDR_ALT,
        0x18, /* ES8311 codec on the same control bus. */
        0x36, /* OV5647 SCCB on the same control bus. */
    };

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        esp_err_t ret = i2c_master_probe(s_ctx.bus_handle, addrs[i], 100);
        ESP_LOGW(TAG, "I2C DIAG probe addr=0x%02X ret=%s", addrs[i], esp_err_to_name(ret));
    }
}

static void mpu_i2c_bus_reset(const char *reason)
{
    if (s_ctx.bus_handle == NULL) {
        return;
    }

    ESP_LOGW(TAG, "MPU6050 I2C bus reset skipped on shared camera/audio bus reason=%s",
             reason != NULL ? reason : "unknown");
    s_ctx.force_wake_check = true;
}

static uint8_t mpu_read_reg_or_ff(uint8_t reg)
{
    uint8_t value = 0xFF;
    (void)mpu_read_regs(reg, &value, 1);
    return value;
}

static esp_err_t mpu_wake_and_verify(void)
{
    esp_err_t last = ESP_FAIL;

    for (int attempt = 0; attempt < 5; attempt++) {
        uint8_t before_pwr1 = mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_1);
        uint8_t before_pwr2 = mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_2);

        last = mpu_write_reg(MPU6050_REG_PWR_MGMT_1, 0x01);
        if (last != ESP_OK) {
            ESP_LOGW(TAG, "MPU6050 wake write pwr1 failed attempt=%d ret=%s", attempt + 1, esp_err_to_name(last));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last = mpu_write_reg(MPU6050_REG_PWR_MGMT_2, 0x00);
        if (last != ESP_OK) {
            ESP_LOGW(TAG, "MPU6050 wake write pwr2 failed attempt=%d ret=%s", attempt + 1, esp_err_to_name(last));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        uint8_t pwr1 = 0xFF;
        uint8_t pwr2 = 0xFF;
        last = mpu_read_regs(MPU6050_REG_PWR_MGMT_1, &pwr1, 1);
        if (last != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last = mpu_read_regs(MPU6050_REG_PWR_MGMT_2, &pwr2, 1);
        if (last != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if ((pwr1 & 0x40) == 0 && pwr2 == 0x00) {
            ESP_LOGI(TAG, "MPU6050 wake ok attempt=%d before=0x%02X/0x%02X after=0x%02X/0x%02X",
                     attempt + 1, before_pwr1, before_pwr2, pwr1, pwr2);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "MPU6050 wake retry %d before=0x%02X/0x%02X after=0x%02X/0x%02X",
                 attempt + 1, before_pwr1, before_pwr2, pwr1, pwr2);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    ESP_LOGW(TAG, "MPU6050 wake verify did not clear sleep bit; continue in diagnostic mode");
    mpu_dump_regs("wake_failed");
    return last == ESP_OK ? ESP_OK : last;
}

static esp_err_t mpu_ensure_awake(void)
{
    uint8_t pwr1 = 0;
    uint8_t pwr2 = 0;
    esp_err_t ret = mpu_read_regs(MPU6050_REG_PWR_MGMT_1, &pwr1, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mpu_read_regs(MPU6050_REG_PWR_MGMT_2, &pwr2, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((pwr1 & 0x40) != 0 || pwr2 != 0x00) {
        ESP_LOGW(TAG, "MPU6050 wake guard pwr1=0x%02X pwr2=0x%02X", pwr1, pwr2);
        ESP_RETURN_ON_ERROR(mpu_wake_and_verify(), TAG, "wake guard failed");
    }
    return ESP_OK;
}

static float compute_tilt_angle(const float accel[3])
{
    float norm = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    if (norm < 0.001f) {
        return 0.0f;
    }
    float cos_theta = fabsf(accel[2]) / norm;
    if (cos_theta > 1.0f) {
        cos_theta = 1.0f;
    }
    return acosf(cos_theta) * 180.0f / 3.14159265f;
}

static float vector_norm3(const float v[3])
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static float vector_angle_deg3(const float a[3], const float b[3])
{
    float an = vector_norm3(a);
    float bn = vector_norm3(b);
    if (an < 0.001f || bn < 0.001f) {
        return 0.0f;
    }
    float cos_theta = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (an * bn);
    if (cos_theta > 1.0f) {
        cos_theta = 1.0f;
    } else if (cos_theta < -1.0f) {
        cos_theta = -1.0f;
    }
    return acosf(cos_theta) * 180.0f / 3.14159265f;
}

static void sensor_reset_calibration(void)
{
    memset(s_ctx.accel_ref, 0, sizeof(s_ctx.accel_ref));
    memset(s_ctx.gyro_bias, 0, sizeof(s_ctx.gyro_bias));
    s_ctx.calibrated = false;
    s_ctx.calibration_count = 0;
    s_ctx.last_tilt_angle = 0.0f;
}

static bool sensor_update_calibration(const float accel[3], const float gyro[3])
{
    if (s_ctx.calibrated) {
        return true;
    }

    float accel_norm = vector_norm3(accel);
    float gyro_norm = vector_norm3(gyro);
    if (accel_norm < MPU6050_CALIBRATION_MIN_ACCEL_G ||
        accel_norm > MPU6050_CALIBRATION_MAX_ACCEL_G ||
        gyro_norm > MPU6050_CALIBRATION_MAX_GYRO_DPS) {
        s_ctx.calibration_count = 0;
        memset(s_ctx.accel_ref, 0, sizeof(s_ctx.accel_ref));
        memset(s_ctx.gyro_bias, 0, sizeof(s_ctx.gyro_bias));
        return false;
    }

    for (int i = 0; i < 3; i++) {
        s_ctx.accel_ref[i] += accel[i];
        s_ctx.gyro_bias[i] += gyro[i];
    }
    s_ctx.calibration_count++;
    if (s_ctx.calibration_count < MPU6050_CALIBRATION_SAMPLES) {
        return false;
    }

    for (int i = 0; i < 3; i++) {
        s_ctx.accel_ref[i] /= (float)s_ctx.calibration_count;
        s_ctx.gyro_bias[i] /= (float)s_ctx.calibration_count;
    }
    s_ctx.calibrated = true;
    ESP_LOGI(TAG,
             "MPU6050 calibration ok samples=%u accel_ref=[%.3f,%.3f,%.3f] gyro_bias=[%.2f,%.2f,%.2f]",
             (unsigned)s_ctx.calibration_count,
             s_ctx.accel_ref[0], s_ctx.accel_ref[1], s_ctx.accel_ref[2],
             s_ctx.gyro_bias[0], s_ctx.gyro_bias[1], s_ctx.gyro_bias[2]);
    return true;
}

static posture_state_t classify_posture(float tilt, float delta)
{
    if (tilt > BODYGUARD_FALL_ANGLE_DEG) {
        return POSTURE_FALL;
    }
    if (delta > BODYGUARD_FAST_ANGLE_DELTA_DEG) {
        return POSTURE_FAST_CHANGE;
    }
    if (tilt > 30.0f) {
        return POSTURE_TILT;
    }
    return POSTURE_NORMAL;
}

static esp_err_t mpu_configure(void)
{
    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(mpu_read_regs(MPU6050_REG_WHO_AM_I, &who, 1), TAG, "read WHO_AM_I failed");
    ESP_RETURN_ON_FALSE(who == MPU6050_WHO_AM_I_VALUE, ESP_ERR_INVALID_RESPONSE, TAG,
                        "MPU6050 unexpected WHO_AM_I: 0x%02X", who);

    ESP_RETURN_ON_ERROR(mpu_wake_and_verify(), TAG, "wake MPU6050 failed");

    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU6050_REG_SMPLRT_DIV, 0x04), TAG, "sample rate config failed");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU6050_REG_CONFIG, 0x03), TAG, "DLPF config failed");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU6050_REG_GYRO_CONFIG, 0x08), TAG, "gyro range config failed");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x08), TAG, "accel range config failed");
    ESP_RETURN_ON_ERROR(mpu_wake_and_verify(), TAG, "verify MPU6050 wake failed");

    uint8_t accel_cfg = mpu_read_reg_or_ff(MPU6050_REG_ACCEL_CONFIG);
    uint8_t gyro_cfg = mpu_read_reg_or_ff(MPU6050_REG_GYRO_CONFIG);
    if (accel_cfg != 0x08 || gyro_cfg != 0x08) {
        ESP_LOGW(TAG, "MPU6050 config verify abnormal accel=0x%02X gyro=0x%02X", accel_cfg, gyro_cfg);
    }
    switch ((accel_cfg >> 3) & 0x03) {
    case 0:
        s_ctx.accel_lsb_per_g = 16384.0f;
        break;
    case 1:
        s_ctx.accel_lsb_per_g = 8192.0f;
        break;
    case 2:
        s_ctx.accel_lsb_per_g = 4096.0f;
        break;
    case 3:
    default:
        s_ctx.accel_lsb_per_g = 2048.0f;
        break;
    }
    switch ((gyro_cfg >> 3) & 0x03) {
    case 0:
        s_ctx.gyro_lsb_per_dps = 131.0f;
        break;
    case 1:
        s_ctx.gyro_lsb_per_dps = 65.5f;
        break;
    case 2:
        s_ctx.gyro_lsb_per_dps = 32.8f;
        break;
    case 3:
    default:
        s_ctx.gyro_lsb_per_dps = 16.4f;
        break;
    }
    ESP_LOGI(TAG, "MPU6050 scale accel_lsb_per_g=%.1f gyro_lsb_per_dps=%.1f",
             s_ctx.accel_lsb_per_g, s_ctx.gyro_lsb_per_dps);
    s_ctx.zero_data_count = 0;
    s_ctx.read_fail_count = 0;
    sensor_reset_calibration();
    return ESP_OK;
}

static bool raw_sample_all_zero(const int16_t accel_raw[3], const int16_t gyro_raw[3])
{
    for (int i = 0; i < 3; i++) {
        if (accel_raw[i] != 0 || gyro_raw[i] != 0) {
            return false;
        }
    }
    return true;
}

static void sensor_try_recover(const char *reason)
{
    ESP_LOGW(TAG, "MPU6050 recover start reason=%s pwr1=0x%02X pwr2=0x%02X accel_cfg=0x%02X gyro_cfg=0x%02X",
             reason,
             mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_1),
             mpu_read_reg_or_ff(MPU6050_REG_PWR_MGMT_2),
             mpu_read_reg_or_ff(MPU6050_REG_ACCEL_CONFIG),
             mpu_read_reg_or_ff(MPU6050_REG_GYRO_CONFIG));
    mpu_i2c_bus_reset(reason);
    esp_err_t cfg_ret = mpu_configure();
    if (cfg_ret == ESP_OK) {
        ESP_LOGI(TAG, "MPU6050 recover ok");
    } else {
        ESP_LOGW(TAG, "MPU6050 recover failed: %s", esp_err_to_name(cfg_ret));
    }
}

static void sensor_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "task_sensor started period=%dms", BODYGUARD_SENSOR_POLL_MS);

    while (true) {
        uint8_t raw[14] = {0};
        sensor_sample_t sample = s_ctx.latest;
        uint64_t now = bodyguard_now_ms();
        esp_err_t ret = ESP_OK;
        if (s_ctx.force_wake_check || now >= s_ctx.next_wake_check_ms) {
            ret = mpu_ensure_awake();
            s_ctx.force_wake_check = false;
            s_ctx.next_wake_check_ms = now + MPU6050_WAKE_CHECK_MS;
        }
        if (ret == ESP_OK) {
            ret = mpu_read_regs(MPU6050_REG_DATA_START, raw, sizeof(raw));
        }
        if (ret == ESP_OK) {
            int16_t accel_raw[3] = {
                (int16_t)((raw[0] << 8) | raw[1]),
                (int16_t)((raw[2] << 8) | raw[3]),
                (int16_t)((raw[4] << 8) | raw[5]),
            };
            int16_t gyro_raw[3] = {
                (int16_t)((raw[8] << 8) | raw[9]),
                (int16_t)((raw[10] << 8) | raw[11]),
                (int16_t)((raw[12] << 8) | raw[13]),
            };

            if (raw_sample_all_zero(accel_raw, gyro_raw)) {
                if (++s_ctx.zero_data_count >= MPU6050_ZERO_RECOVER_COUNT) {
                    sensor_try_recover("all_zero_data");
                }
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(BODYGUARD_SENSOR_POLL_MS));
                continue;
            }
            s_ctx.zero_data_count = 0;

            float accel_scale = s_ctx.accel_lsb_per_g > 1.0f ? s_ctx.accel_lsb_per_g : 16384.0f;
            float gyro_scale = s_ctx.gyro_lsb_per_dps > 1.0f ? s_ctx.gyro_lsb_per_dps : 131.0f;
            float accel_cal[3] = {0};
            float gyro_cal[3] = {0};
            for (int i = 0; i < 3; i++) {
                accel_cal[i] = (float)accel_raw[i] / accel_scale;
                gyro_cal[i] = (float)gyro_raw[i] / gyro_scale;
            }
            bool calibrated = sensor_update_calibration(accel_cal, gyro_cal);
            for (int i = 0; i < 3; i++) {
                sample.accel[i] = accel_cal[i];
                sample.gyro[i] = calibrated ? (gyro_cal[i] - s_ctx.gyro_bias[i]) : gyro_cal[i];
            }
            sample.tilt_angle = calibrated ? vector_angle_deg3(sample.accel, s_ctx.accel_ref) : compute_tilt_angle(sample.accel);
            sample.angle_delta = fabsf(sample.tilt_angle - s_ctx.last_tilt_angle);
            sample.posture = classify_posture(sample.tilt_angle, sample.angle_delta);
            sample.timestamp_ms = bodyguard_now_ms();
            s_ctx.last_tilt_angle = sample.tilt_angle;
            s_ctx.latest = sample;

            if (xQueueSend(s_ctx.output_queue, &sample, 0) != pdTRUE) {
                sensor_sample_t drop;
                xQueueReceive(s_ctx.output_queue, &drop, 0);
                xQueueSend(s_ctx.output_queue, &sample, 0);
            }
        } else {
            ESP_LOGW(TAG, "MPU6050 read failed: %s", esp_err_to_name(ret));
            if (++s_ctx.read_fail_count >= MPU6050_READ_RECOVER_COUNT) {
                sensor_try_recover("read_error");
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(BODYGUARD_SENSOR_POLL_MS));
    }
}

esp_err_t sensor_init(QueueHandle_t output_queue)
{
    ESP_RETURN_ON_FALSE(output_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "sensor queue is null");
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.output_queue = output_queue;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BODYGUARD_I2C_PORT,
        .sda_io_num = BODYGUARD_I2C_SDA_GPIO,
        .scl_io_num = BODYGUARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BODYGUARD_MPU6050_ADDR,
        .scl_speed_hz = BODYGUARD_I2C_PORT_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_ctx.bus_handle), TAG, "create I2C bus failed");
    mpu_probe_known_addresses();
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_ctx.bus_handle, &dev_cfg, &s_ctx.mpu_handle),
                        TAG, "add MPU6050 failed");
    esp_err_t cfg_ret = ESP_FAIL;
    for (int attempt = 0; attempt < 10; attempt++) {
        mpu_dump_regs("before_config");
        cfg_ret = mpu_configure();
        mpu_dump_regs("after_config");
        if (cfg_ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "MPU6050 configure retry %d/10: %s", attempt + 1, esp_err_to_name(cfg_ret));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_RETURN_ON_ERROR(cfg_ret, TAG, "configure MPU6050 failed");

    s_ctx.initialized = true;
    s_ctx.force_wake_check = true;
    ESP_LOGI(TAG, "MPU6050 initialized SDA=%d SCL=%d", BODYGUARD_I2C_SDA_GPIO, BODYGUARD_I2C_SCL_GPIO);
    return ESP_OK;
}

esp_err_t sensor_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "sensor not initialized");
    if (s_ctx.task_handle != NULL) {
        return ESP_OK;
    }
    /*
     * OV5647 SCCB and ES8311 share this I2C bus during init. Re-apply the MPU
     * configuration after those init phases, before the camera capture task
     * starts, so the posture sensor is the last configured control device.
     */
    mpu_i2c_bus_reset("sensor_start");
    mpu_dump_regs("sensor_start_before_config");
    ESP_RETURN_ON_ERROR(mpu_configure(), TAG, "late configure MPU6050 failed");
    mpu_dump_regs("sensor_start_after_config");
    s_ctx.force_wake_check = true;
    BaseType_t ok = xTaskCreate(sensor_task, "task_sensor", BODYGUARD_TASK_STACK_MEDIUM, NULL,
                                BODYGUARD_TASK_PRIO_SENSOR, &s_ctx.task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

i2c_master_bus_handle_t sensor_get_i2c_bus(void)
{
    return s_ctx.bus_handle;
}

sensor_sample_t sensor_get_latest(void)
{
    return s_ctx.latest;
}
