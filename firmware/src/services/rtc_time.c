/**
 * @file    rtc_time.c
 * @brief   RTC 日历与 UTC 时间的纯换算实现（见 rtc_time.h）。
 */
#include "rtc_time.h"

#include <stddef.h>

#define SECONDS_PER_DAY 86400ULL
/** 1970-01-01 到 2000-01-01 的天数（2000-01-01T00:00:00Z = 946684800 s） */
#define UNIX_DAY_AT_YEAR_2000 10957ULL
/** 1970-01-01 为周四；周几编号 1 = 周一时的偏移 */
#define UNIX_EPOCH_WEEKDAY_OFFSET 3U

/** 校准脉冲 → 0.01 ppm 的换算：1 脉冲 / 2^20 个 RTCCLK = 1562500/16384 个 0.01 ppm */
#define CALIB_NUM 1562500LL
#define CALIB_DEN 16384LL
/** CALP 在 2^20 窗口内插入的脉冲数 */
#define CALIB_PLUS_PULSES 512LL
/** CALM[8:0] 的最大值 */
#define CALIB_MINUS_MAX 511LL
/** 协议允许的校准请求上限（0.01 ppm） */
#define CALIB_CENTI_PPM_MAX 30000LL

static bool is_leap_year(uint16_t year)
{
    /* NOTE: 2000..2099 内没有整百年，故只需判 4 的倍数（2000 年能被 400 整除） */
    return (year % 4U) == 0U;
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U,
                                     31U, 31U, 30U, 31U, 30U, 31U};
    if (month < 1U || month > 12U) {
        return 0U;
    }
    if (month == 2U && is_leap_year(year)) {
        return 29U;
    }
    return days[month - 1U];
}

/** 就近取整的整数除法（den > 0；num 可为负） */
static int64_t div_round_nearest(int64_t num, int64_t den)
{
    if (num >= 0) {
        return (num + den / 2) / den;
    }
    return -((-num + den / 2) / den);
}

bool rtc_time_bcd_to_bin(uint8_t bcd, uint8_t *bin_out)
{
    const uint8_t hi = (uint8_t)(bcd >> 4);
    const uint8_t lo = (uint8_t)(bcd & 0x0FU);
    if (hi > 9U || lo > 9U) {
        return false;
    }
    if (bin_out != NULL) {
        *bin_out = (uint8_t)(hi * 10U + lo);
    }
    return true;
}

uint8_t rtc_time_bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10U) << 4) | (bin % 10U));
}

bool rtc_time_from_unix(uint64_t unix_s, rtc_time_civil_t *out)
{
    if (out == NULL) {
        return false;
    }

    uint64_t days = unix_s / SECONDS_PER_DAY;
    uint32_t rem = (uint32_t)(unix_s % SECONDS_PER_DAY);
    if (days < UNIX_DAY_AT_YEAR_2000) {
        return false; /* 1970..1999 超出 RTC 年字段 */
    }
    days -= UNIX_DAY_AT_YEAR_2000;

    uint16_t year = RTC_TIME_YEAR_MIN;
    for (;;) {
        const uint16_t year_days = is_leap_year(year) ? 366U : 365U;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        year++;
        if (year > RTC_TIME_YEAR_MAX) {
            return false;
        }
    }

    uint8_t month = 1U;
    while (days >= days_in_month(year, month)) {
        days -= days_in_month(year, month);
        month++;
    }

    out->year    = year;
    out->month   = month;
    out->day     = (uint8_t)(days + 1U);
    out->weekday = (uint8_t)(((unix_s / SECONDS_PER_DAY) + UNIX_EPOCH_WEEKDAY_OFFSET) % 7U + 1U);
    out->hour    = (uint8_t)(rem / 3600U);
    out->minute  = (uint8_t)((rem % 3600U) / 60U);
    out->second  = (uint8_t)(rem % 60U);
    return true;
}

bool rtc_time_to_unix(const rtc_time_civil_t *civil, uint64_t *unix_s_out)
{
    if (civil == NULL || unix_s_out == NULL) {
        return false;
    }
    if (civil->year < RTC_TIME_YEAR_MIN || civil->year > RTC_TIME_YEAR_MAX ||
        civil->month < 1U || civil->month > 12U ||
        civil->hour > 23U || civil->minute > 59U || civil->second > 59U ||
        civil->weekday < 1U || civil->weekday > 7U) {
        return false;
    }
    if (civil->day < 1U || civil->day > days_in_month(civil->year, civil->month)) {
        return false;
    }

    uint64_t days = 0U;
    for (uint16_t y = RTC_TIME_YEAR_MIN; y < civil->year; y++) {
        days += is_leap_year(y) ? 366U : 365U;
    }
    for (uint8_t m = 1U; m < civil->month; m++) {
        days += days_in_month(civil->year, m);
    }
    days += (uint64_t)(civil->day - 1U);

    *unix_s_out = (UNIX_DAY_AT_YEAR_2000 + days) * SECONDS_PER_DAY +
                  (uint64_t)civil->hour * 3600U +
                  (uint64_t)civil->minute * 60U +
                  (uint64_t)civil->second;
    return true;
}

bool rtc_time_ticks_from_ms(uint16_t ms, uint8_t *ticks_out)
{
    if (ticks_out == NULL || ms > 999U) {
        return false;
    }
    *ticks_out = (uint8_t)(((uint32_t)ms * 256U) / 1000U);
    return true;
}

int32_t rtc_time_ms_from_ticks(int32_t ticks)
{
    const int64_t num = (int64_t)ticks * 1000;
    int64_t ms = num / 256;
    if (num < 0 && (num % 256) != 0) {
        ms -= 1; /* 向下取整，保证读数不早于真实相位 */
    }
    return (int32_t)ms;
}

bool rtc_time_calib_from_centi_ppm(int32_t centi_ppm, rtc_time_calib_t *out)
{
    if (out == NULL || (int64_t)centi_ppm < -CALIB_CENTI_PPM_MAX ||
        (int64_t)centi_ppm > CALIB_CENTI_PPM_MAX) {
        return false;
    }

    /* 目标脉冲数 = centi_ppm * 16384 / 1562500；以 1/1562500 为公共分母比较误差 */
    const int64_t want_num = (int64_t)centi_ppm * CALIB_DEN;
    int64_t best_error = 0;
    rtc_time_calib_t best = { false, 0U };
    bool have_best = false;

    for (int calp = 0; calp <= 1; calp++) {
        const int64_t pulses = CALIB_PLUS_PULSES * (int64_t)calp;
        int64_t minus = div_round_nearest(pulses * CALIB_NUM - want_num, CALIB_NUM);
        if (minus < 0) {
            minus = 0;
        }
        if (minus > CALIB_MINUS_MAX) {
            minus = CALIB_MINUS_MAX;
        }
        const int64_t error = (pulses - minus) * CALIB_NUM - want_num;
        const int64_t abs_error = (error < 0) ? -error : error;
        if (!have_best || abs_error < best_error) {
            best_error = abs_error;
            best.plus = (calp != 0);
            best.minus = (uint16_t)minus;
            have_best = true;
        }
    }

    *out = best;
    return true;
}

int32_t rtc_time_calib_to_centi_ppm(const rtc_time_calib_t *calib)
{
    if (calib == NULL) {
        return 0;
    }
    const int64_t pulses = (calib->plus ? CALIB_PLUS_PULSES : 0) - (int64_t)calib->minus;
    return (int32_t)div_round_nearest(pulses * CALIB_NUM, CALIB_DEN);
}
