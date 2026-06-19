/**
 * @file alert.c
 * @brief Local alarm control for the T5 safety monitor display terminal.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "data_def.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */
typedef struct {
    bool ready;
    bool active;
    uint8_t last_level;
    TIMER_ID beep_timer;
    TDL_LED_HANDLE_T led;
} alert_ctx_t;

static alert_ctx_t s_alert = {0};

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief 蜂鸣器周期回调。
 * @param[in] timer_id 定时器 ID。
 * @param[in] arg 用户参数。
 * @return none
 */
static void __beep_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    if (s_alert.active) {
        (void)ai_audio_player_alert(AI_AUDIO_ALERT_NETWORK_FAIL);
    }
}

/**
 * @brief 获取当前报警等级。
 * @param[in] state 应用状态。
 * @return 0=无报警，1=预警，2=紧急。
 */
static uint8_t __alert_level(const t5_app_state_t *state)
{
    if (state == NULL) {
        return 0;
    }
    if (state->packet.sys_state == T5_SYS_EMERGENCY) {
        return 2;
    }
    if (state->packet.sys_state == T5_SYS_PRE_ALERT) {
        return 1;
    }
    return 0;
}

OPERATE_RET alert_init(void)
{
    if (s_alert.ready) {
        return OPRT_OK;
    }

    s_alert.led = tdl_led_find_dev(LED_NAME);
    if (s_alert.led != NULL) {
        (void)tdl_led_open(s_alert.led);
    }

    (void)tal_sw_timer_create(__beep_timer_cb, NULL, &s_alert.beep_timer);
    (void)ai_audio_player_init();
    s_alert.ready = true;
    return OPRT_OK;
}

void alert_refresh(void)
{
    t5_app_state_t state;
    uint8_t level;

    if (!s_alert.ready) {
        return;
    }

    if (app_state_get(&state) != OPRT_OK) {
        return;
    }

    level = __alert_level(&state);
    if (level == s_alert.last_level) {
        return;
    }

    s_alert.last_level = level;
    if (level == 2) {
        s_alert.active = true;
        if (s_alert.led != NULL) {
            (void)tdl_led_blink(s_alert.led, &(TDL_LED_BLINK_CFG_T){
                .cnt = TDL_BLINK_FOREVER,
                .start_stat = TDL_LED_ON,
                .end_stat = TDL_LED_OFF,
                .first_half_cycle_time = 120,
                .latter_half_cycle_time = 120,
            });
        }
        (void)tal_sw_timer_start(s_alert.beep_timer, 2500, TAL_TIMER_CYCLE);
        T5_LOG_I("alert level -> emergency");
        return;
    }

    if (level == 1) {
        s_alert.active = false;
        if (s_alert.led != NULL) {
            (void)tdl_led_blink(s_alert.led, &(TDL_LED_BLINK_CFG_T){
                .cnt = TDL_BLINK_FOREVER,
                .start_stat = TDL_LED_ON,
                .end_stat = TDL_LED_OFF,
                .first_half_cycle_time = 300,
                .latter_half_cycle_time = 300,
            });
        }
        (void)tal_sw_timer_stop(s_alert.beep_timer);
        T5_LOG_I("alert level -> warning");
        return;
    }

    s_alert.active = false;
    if (s_alert.led != NULL) {
        (void)tdl_led_set_status(s_alert.led, TDL_LED_ON);
    }
    (void)tal_sw_timer_stop(s_alert.beep_timer);
    T5_LOG_I("alert level -> normal");
}
