/**
 * @file    fake_plug_detect.c
 * @brief   plug_detect 的主机侧测试替身（见 fake_plug_detect.h）。
 */
#include "fake_plug_detect.h"

#include "plug_detect.h"

static bool s_pending = false;

void fake_plug_detect_reset(void)
{
    s_pending = false;
}

void fake_plug_detect_set_pending(void)
{
    s_pending = true;
}

/* ---- 与 plug_detect.h 原型一致的测试实现 ---- */

void plug_detect_init(void)
{
    s_pending = false;
}

bool plug_detect_wake_pending(void)
{
    const bool pending = s_pending;
    s_pending = false;
    return pending;
}
