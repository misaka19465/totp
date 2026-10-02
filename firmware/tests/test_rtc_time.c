/**
 * @file    test_rtc_time.c
 * @brief   rtc_time 单元测试：UTC↔日历换算、BCD、1/256 s 量化与校准折算。
 *
 * 时间向量用 serial-protocol.md §8 的 2026-10-02T00:00:00Z（= 1790899200000 ms）
 * 与 2000-02-29（闰日）等边界逐值对照；另用 2000..2099 全时段逐日往返
 * 覆盖闰年与月长逻辑。
 */
#include "test_util.h"

#include "rtc_time.h"

static bool civil_is(const rtc_time_civil_t *civil, uint16_t year, uint8_t month,
                     uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                     uint8_t second)
{
    return civil->year == year && civil->month == month && civil->day == day &&
           civil->weekday == weekday && civil->hour == hour &&
           civil->minute == minute && civil->second == second;
}

static void test_bcd(void)
{
    uint8_t value = 0U;

    CHECK(rtc_time_bin_to_bcd(0U) == 0x00U);
    CHECK(rtc_time_bin_to_bcd(9U) == 0x09U);
    CHECK(rtc_time_bin_to_bcd(10U) == 0x10U);
    CHECK(rtc_time_bin_to_bcd(59U) == 0x59U);
    CHECK(rtc_time_bin_to_bcd(99U) == 0x99U);

    CHECK(rtc_time_bcd_to_bin(0x00U, &value) && value == 0U);
    CHECK(rtc_time_bcd_to_bin(0x09U, &value) && value == 9U);
    CHECK(rtc_time_bcd_to_bin(0x59U, &value) && value == 59U);
    CHECK(rtc_time_bcd_to_bin(0x99U, &value) && value == 99U);
    CHECK(!rtc_time_bcd_to_bin(0x0AU, &value));
    CHECK(!rtc_time_bcd_to_bin(0xA0U, &value));
    CHECK(!rtc_time_bcd_to_bin(0xFFU, &value));
    CHECK(rtc_time_bcd_to_bin(0x99U, NULL)); /* 仅校验 */

    for (uint8_t v = 0U; v <= 99U; v++) {
        CHECK(rtc_time_bcd_to_bin(rtc_time_bin_to_bcd(v), &value) && value == v);
    }
}

static void test_unix_vectors(void)
{
    rtc_time_civil_t civil;
    uint64_t unix_s = 0U;

    /* 2000-01-01T00:00:00Z（RTC 年字段下限） */
    CHECK(rtc_time_from_unix(946684800ULL, &civil));
    CHECK(civil_is(&civil, 2000U, 1U, 1U, 6U, 0U, 0U, 0U));
    CHECK(rtc_time_to_unix(&civil, &unix_s) && unix_s == 946684800ULL);

    /* 2000-02-29（闰日） */
    CHECK(rtc_time_from_unix(951782400ULL, &civil));
    CHECK(civil_is(&civil, 2000U, 2U, 29U, 2U, 0U, 0U, 0U));

    /* serial-protocol.md §8 的对时向量：2026-10-02T00:00:00Z */
    CHECK(rtc_time_from_unix(1790899200ULL, &civil));
    CHECK(civil_is(&civil, 2026U, 10U, 2U, 5U, 0U, 0U, 0U));
    CHECK(rtc_time_to_unix(&civil, &unix_s) && unix_s == 1790899200ULL);

    CHECK(rtc_time_from_unix(1790902861ULL, &civil));
    CHECK(civil_is(&civil, 2026U, 10U, 2U, 5U, 1U, 1U, 1U));

    /* 2099-12-31T23:59:59Z（RTC 年字段上限） */
    CHECK(rtc_time_from_unix(4102444799ULL, &civil));
    CHECK(civil_is(&civil, 2099U, 12U, 31U, 4U, 23U, 59U, 59U));
    CHECK(rtc_time_to_unix(&civil, &unix_s) && unix_s == 4102444799ULL);

    /* 超出 2000..2099 的边界 */
    CHECK(!rtc_time_from_unix(946684799ULL, &civil));  /* 1999-12-31T23:59:59Z */
    CHECK(!rtc_time_from_unix(4102444800ULL, &civil)); /* 2100-01-01T00:00:00Z */
    CHECK(!rtc_time_from_unix(0ULL, &civil));
    CHECK(!rtc_time_from_unix(946684800ULL, NULL));
}

static void test_unix_sweep(void)
{
    /* 2000-01-01..2099-12-31 逐日往返：覆盖全部月份长度与闰年分支。
     * 失败按天累计、只报一条断言，避免 3.6 万条 CHECK 淹没其他用例。 */
    uint32_t bad_days = 0U;
    uint32_t first_bad_day = 0U;
    for (uint32_t day = 0U; day <= 36524U; day++) {
        const uint64_t unix_s = 946684800ULL + (uint64_t)day * 86400ULL;
        rtc_time_civil_t civil;
        uint64_t back = 0U;
        const bool ok = rtc_time_from_unix(unix_s, &civil) &&
                        civil.weekday == (uint8_t)(((day + 5U) % 7U) + 1U) &&
                        rtc_time_to_unix(&civil, &back) && back == unix_s;
        if (!ok) {
            if (bad_days == 0U) {
                first_bad_day = day;
            }
            bad_days++;
        }
    }
    CHECK(bad_days == 0U);
    if (bad_days != 0U) {
        printf("  首个不一致的日序号：%u（共 %u 天）\n", first_bad_day, bad_days);
    }
}

static void test_invalid_civil(void)
{
    rtc_time_civil_t civil = { 2026U, 10U, 2U, 5U, 12U, 0U, 0U };
    uint64_t unix_s = 0U;

    CHECK(rtc_time_to_unix(&civil, &unix_s));

    civil = (rtc_time_civil_t){ 2023U, 2U, 29U, 3U, 0U, 0U, 0U }; /* 非闰年 2 月 29 */
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 1U, 0U, 4U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 4U, 31U, 5U, 0U, 0U, 0U }; /* 4 月只有 30 天 */
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 0U, 1U, 4U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 13U, 1U, 4U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 10U, 2U, 5U, 24U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 10U, 2U, 5U, 0U, 60U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 10U, 2U, 5U, 0U, 0U, 60U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 10U, 2U, 0U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2026U, 10U, 2U, 8U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 2100U, 1U, 1U, 5U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    civil = (rtc_time_civil_t){ 1999U, 1U, 1U, 5U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&civil, &unix_s));
    CHECK(!rtc_time_to_unix(NULL, &unix_s));

    rtc_time_civil_t good = { 2026U, 10U, 2U, 5U, 0U, 0U, 0U };
    CHECK(!rtc_time_to_unix(&good, NULL));
}

static void test_subseconds(void)
{
    uint8_t ticks = 0U;

    CHECK(rtc_time_ticks_from_ms(0U, &ticks) && ticks == 0U);
    CHECK(rtc_time_ticks_from_ms(3U, &ticks) && ticks == 0U);
    CHECK(rtc_time_ticks_from_ms(4U, &ticks) && ticks == 1U);
    CHECK(rtc_time_ticks_from_ms(62U, &ticks) && ticks == 15U);
    CHECK(rtc_time_ticks_from_ms(999U, &ticks) && ticks == 255U);
    CHECK(!rtc_time_ticks_from_ms(1000U, &ticks));
    CHECK(!rtc_time_ticks_from_ms(0U, NULL));

    CHECK(rtc_time_ms_from_ticks(0) == 0);
    CHECK(rtc_time_ms_from_ticks(1) == 3);
    CHECK(rtc_time_ms_from_ticks(255) == 996);
    /* 移位后 SS > PREDIV_S：相位为负，读数是"比日历秒字段早" */
    CHECK(rtc_time_ms_from_ticks(-1) == -4);
    CHECK(rtc_time_ms_from_ticks(-256) == -1000);

    /* 量化误差不超过 4 ms（1/256 s 的整数毫秒上界） */
    for (uint16_t ms = 0U; ms <= 999U; ms++) {
        CHECK(rtc_time_ticks_from_ms(ms, &ticks));
        const int32_t back = rtc_time_ms_from_ticks((int32_t)ticks);
        CHECK(back <= (int32_t)ms && (int32_t)ms - back <= 4);
    }
}

static void test_calibration(void)
{
    rtc_time_calib_t regs;

    CHECK(rtc_time_calib_from_centi_ppm(0, &regs));
    CHECK(!regs.plus && regs.minus == 0U);
    CHECK(rtc_time_calib_to_centi_ppm(&regs) == 0);

    /* ±300 ppm 边界：0.9537 ppm 一档，取最接近的档位 */
    CHECK(rtc_time_calib_from_centi_ppm(30000, &regs));
    CHECK(regs.plus && regs.minus == 197U);
    CHECK(rtc_time_calib_to_centi_ppm(&regs) == 30041);
    CHECK(rtc_time_calib_from_centi_ppm(-30000, &regs));
    CHECK(!regs.plus && regs.minus == 315U);
    CHECK(rtc_time_calib_to_centi_ppm(&regs) == -30041);

    /* 寄存器档位回读：CALP 满量程 / 单档负修正 */
    regs = (rtc_time_calib_t){ true, 0U };
    CHECK(rtc_time_calib_to_centi_ppm(&regs) == 48828);
    regs = (rtc_time_calib_t){ false, 1U };
    CHECK(rtc_time_calib_to_centi_ppm(&regs) == -95);

    CHECK(!rtc_time_calib_from_centi_ppm(30001, &regs));
    CHECK(!rtc_time_calib_from_centi_ppm(-30001, &regs));
    CHECK(!rtc_time_calib_from_centi_ppm(0, NULL));
    CHECK(rtc_time_calib_to_centi_ppm(NULL) == 0);

    /* 全范围扫描：量化误差不超过半档（48 个 0.01 ppm） */
    for (int32_t request = -30000; request <= 30000; request += 37) {
        CHECK(rtc_time_calib_from_centi_ppm(request, &regs));
        const int32_t applied = rtc_time_calib_to_centi_ppm(&regs);
        const int32_t error = applied - request;
        CHECK(error >= -48 && error <= 48);
    }
}

void test_rtc_time(void)
{
    test_bcd();
    test_unix_vectors();
    test_unix_sweep();
    test_invalid_civil();
    test_subseconds();
    test_calibration();
}
