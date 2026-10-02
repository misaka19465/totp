/**
 * @file    test_watchdog.c
 * @brief   watchdog 纯逻辑测试：复位原因标志的优先级归并（线上语义）。
 */
#include "test_util.h"

#include "watchdog.h"

void test_watchdog(void)
{
    /* 多个标志同时置位时按优先级取一 */
    CHECK_U32(watchdog_classify_reset(true, true, true, true), WATCHDOG_RESET_IWDG);
    CHECK_U32(watchdog_classify_reset(false, true, true, true), WATCHDOG_RESET_SOFTWARE);
    /* 上电瞬间 POR 与 PIN 同时置位：电源类优先 */
    CHECK_U32(watchdog_classify_reset(false, false, true, true), WATCHDOG_RESET_POR);
    CHECK_U32(watchdog_classify_reset(false, false, false, true), WATCHDOG_RESET_OTHER);

    /* 单一标志 */
    CHECK_U32(watchdog_classify_reset(true, false, false, false), WATCHDOG_RESET_IWDG);
    CHECK_U32(watchdog_classify_reset(false, true, false, false), WATCHDOG_RESET_SOFTWARE);
    CHECK_U32(watchdog_classify_reset(false, false, true, false), WATCHDOG_RESET_POR);
    CHECK_U32(watchdog_classify_reset(false, false, false, true), WATCHDOG_RESET_OTHER);
    CHECK_U32(watchdog_classify_reset(false, false, false, false), WATCHDOG_RESET_UNKNOWN);
}
