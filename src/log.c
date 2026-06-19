/**
 * log.c - T5 AI风险终端 日志初始化模块
 *
 * 职责：
 *   初始化 TuyaOpen SDK 的日志子系统，设置日志级别为 DEBUG，
 *   输出回调绑定到 tkl_log_output（串口输出）。
 *
 * 日志缓冲区大小：4096字节
 * 日志级别：TAL_LOG_LEVEL_DEBUG（开发阶段使用最详细级别）
 */

#include "config.h"

#include "tkl_output.h"

void app_log_init(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("[risk-terminal] Log system initialized");
    TUYA_LOG_I("Log level: DEBUG, buffer: 4096 bytes");
}
