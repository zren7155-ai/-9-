#include "data_def.h"

void user_main(void)
{
    bodyguard_t5_app_run();
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    /* 提前初始化日志，确保启动时就能看到日志输出 */
    app_log_init();
    PR_NOTICE("=== T5 AI Risk Terminal Starting ===");

    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
#if defined(PLATFORM_T5) && (PLATFORM_T5 == 1)
    extern VOID_T tkl_system_psram_malloc_force_set(BOOL_T enable);
    tkl_system_psram_malloc_force_set(TRUE);
#endif

    /* 关键：在创建应用线程之前初始化日志系统 */
    /* 这样可以确保从启动开始就能看到日志输出 */
    app_log_init();
    PR_NOTICE("=== T5 AI Risk Terminal Starting ===");

    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
