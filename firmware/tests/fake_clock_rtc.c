/**
 * @file    fake_clock_rtc.c
 * @brief   clock_rtc 的主机侧测试替身（见 fake_clock_rtc.h）。
 */
#include "fake_clock_rtc.h"

#include <stddef.h>

#include "clock_rtc.h"

static bool s_ready = true;
static uint64_t s_now_ms = 0U;
static uint8_t s_time_flags = 0U;
static uint32_t s_last_set_age_s = 0U;
static bool s_calib_active = false;
static bool s_fail_set_time = false;
static bool s_fail_set_calib = false;
static uint64_t s_last_set_time_arg = 0U;
static int16_t s_last_calib_arg = 0;

void fake_clock_rtc_reset(void)
{
    s_ready = true;
    s_now_ms = 0U;
    s_time_flags = 0U;
    s_last_set_age_s = 0U;
    s_calib_active = false;
    s_fail_set_time = false;
    s_fail_set_calib = false;
    s_last_set_time_arg = 0U;
    s_last_calib_arg = 0;
}

void fake_clock_rtc_set_ready(bool ready)
{
    s_ready = ready;
}

void fake_clock_rtc_set_now_ms(uint64_t ms)
{
    s_now_ms = ms;
}

void fake_clock_rtc_set_time_flags(uint8_t flags)
{
    s_time_flags = flags;
}

void fake_clock_rtc_set_last_set_age_s(uint32_t age_s)
{
    s_last_set_age_s = age_s;
}

void fake_clock_rtc_set_calib_active(bool active)
{
    s_calib_active = active;
}

void fake_clock_rtc_fail_set_time(bool fail)
{
    s_fail_set_time = fail;
}

void fake_clock_rtc_fail_set_calib(bool fail)
{
    s_fail_set_calib = fail;
}

uint64_t fake_clock_rtc_last_set_time_arg(void)
{
    return s_last_set_time_arg;
}

int16_t fake_clock_rtc_last_calib_arg(void)
{
    return s_last_calib_arg;
}

/* ---- 与 clock_rtc.h 原型一致的测试实现 ---- */

bool clock_rtc_init(void)
{
    return s_ready;
}

bool clock_rtc_ready(void)
{
    return s_ready;
}

uint64_t clock_rtc_now_ms(void)
{
    return s_now_ms;
}

uint8_t clock_rtc_time_flags(void)
{
    return s_time_flags;
}

uint32_t clock_rtc_last_set_age_s(void)
{
    return s_last_set_age_s;
}

bool clock_rtc_set_unix_ms(uint64_t unix_ms, uint64_t *readback_ms)
{
    if (!s_ready || s_fail_set_time) {
        return false;
    }
    s_last_set_time_arg = unix_ms;
    if (readback_ms != NULL) {
        *readback_ms = unix_ms;
    }
    return true;
}

bool clock_rtc_set_calib(int16_t centi_ppm, int16_t *applied_centi_ppm)
{
    if (!s_ready || s_fail_set_calib) {
        return false;
    }
    s_last_calib_arg = centi_ppm;
    if (applied_centi_ppm != NULL) {
        *applied_centi_ppm = centi_ppm;
    }
    return true;
}

bool clock_rtc_calib_active(void)
{
    return s_calib_active;
}
