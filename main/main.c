/**
 * @file main.c
 * @brief Main entry for the T5 AI Board safety monitor terminal.
 * @version 0.1
 * @date 2026-05-07
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "data_def.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * File-scope variables
 * --------------------------------------------------------------------------- */
static THREAD_HANDLE s_ble_thread = NULL;
static THREAD_HANDLE s_ui_thread = NULL;
static THREAD_HANDLE s_alert_thread = NULL;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief BLE 客户端任务，负责扫描、连接、订阅和断线重连。
 * @param[in] arg 任务参数。
 * @return none
 */
static void __ble_client_task(void *arg)
{
    (void)arg;

    for (;;) {
        ble_client_start();
        tal_system_sleep(200);
    }
}

/**
 * @brief UI 任务，负责从共享状态刷新屏幕。
 * @param[in] arg 任务参数。
 * @return none
 */
static void __ui_task(void *arg)
{
    (void)arg;

    for (;;) {
        ui_refresh();
        tal_system_sleep(80);
    }
}

/**
 * @brief 报警任务，负责声光报警状态刷新。
 * @param[in] arg 任务参数。
 * @return none
 */
static void __alert_task(void *arg)
{
    (void)arg;

    for (;;) {
        alert_refresh();
        tal_system_sleep(100);
    }
}

/**
 * @brief 主应用循环入口。
 * @return none
 */
void bodyguard_t5_app_run(void)
{
    THREAD_CFG_T thread_cfg;

    app_log_init();
    T5_LOG_I("bodyguard_t5_app_run start");
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    if (board_register_hardware() != OPRT_OK) {
        T5_LOG_E("board_register_hardware failed");
    }
    T5_LOG_I("board hardware ready");

    (void)app_state_init();
    (void)ble_client_init();
    (void)ui_init();
    (void)alert_init();
    T5_LOG_I("modules ready, creating tasks");

    memset(&thread_cfg, 0, sizeof(thread_cfg));
    thread_cfg.priority = T5_BLE_TASK_PRIO;
    thread_cfg.stackDepth = T5_BLE_TASK_STACK_SIZE;
    thread_cfg.thrdname = "ble_client_task";
    (void)tal_thread_create_and_start(&s_ble_thread, NULL, NULL, __ble_client_task, NULL, &thread_cfg);
    T5_LOG_I("ble_client_task created");

    memset(&thread_cfg, 0, sizeof(thread_cfg));
    thread_cfg.priority = T5_UI_TASK_PRIO;
    thread_cfg.stackDepth = T5_UI_TASK_STACK_SIZE;
    thread_cfg.thrdname = "ui_task";
    (void)tal_thread_create_and_start(&s_ui_thread, NULL, NULL, __ui_task, NULL, &thread_cfg);
    T5_LOG_I("ui_task created");

    memset(&thread_cfg, 0, sizeof(thread_cfg));
    thread_cfg.priority = T5_ALERT_TASK_PRIO;
    thread_cfg.stackDepth = T5_ALERT_TASK_STACK_SIZE;
    thread_cfg.thrdname = "alert_task";
    (void)tal_thread_create_and_start(&s_alert_thread, NULL, NULL, __alert_task, NULL, &thread_cfg);
    T5_LOG_I("alert_task created");

    for (;;) {
        tal_system_sleep(1000);
    }
}
