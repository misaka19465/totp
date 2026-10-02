/**
 * @file    test_state_indicator.c
 * @brief   state_indicator 单元测试：状态点语义与并发规则。
 */
#include "test_util.h"

#include "clock_rtc.h"
#include "lcd_render.h"
#include "state_indicator.h"

static state_indicator_state_t make(bool seed, uint8_t time_flags,
                                    battery_level_t level, bool fault)
{
    state_indicator_state_t state;
    state.seed_present = seed;
    state.time_flags = time_flags;
    state.battery_level = level;
    state.fault_latched = fault;
    return state;
}

void test_state_indicator(void)
{
    const state_indicator_state_t good =
        make(true, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_NORMAL, false);

    /* 一切正常：无点、显示数字 */
    CHECK_U32(state_indicator_dot_mask(&good, 0U), 0U);
    CHECK(!state_indicator_digits_blanked(&good));

    /* 未写入种子：DOT1 亮，数字位留空（无码可算） */
    const state_indicator_state_t no_seed =
        make(false, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_NORMAL, false);
    CHECK_U32(state_indicator_dot_mask(&no_seed, 0U), LCD_DOT_NO_SEED);
    CHECK(state_indicator_digits_blanked(&no_seed));

    /* 时间不可信：NEVER_SET 与 STALE 都点 DOT2，且数字位留空
       （按错误走时算出的码必然无效，不得显示） */
    const state_indicator_state_t never_set =
        make(true, CLOCK_RTC_TIME_NEVER_SET, BATTERY_LEVEL_NORMAL, false);
    const state_indicator_state_t stale =
        make(true, CLOCK_RTC_TIME_STALE, BATTERY_LEVEL_NORMAL, false);
    CHECK_U32(state_indicator_dot_mask(&never_set, 0U), LCD_DOT_NEED_SYNC);
    CHECK_U32(state_indicator_dot_mask(&stale, 0U), LCD_DOT_NEED_SYNC);
    CHECK(state_indicator_digits_blanked(&never_set));
    CHECK(state_indicator_digits_blanked(&stale));

    /* 低电量：DOT4 常亮，与秒相位无关 */
    const state_indicator_state_t low =
        make(true, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_LOW, false);
    CHECK_U32(state_indicator_dot_mask(&low, 0U), LCD_DOT_LOW_BAT);
    CHECK_U32(state_indicator_dot_mask(&low, 1U), LCD_DOT_LOW_BAT);

    /* 极低电量：DOT4 按 1 Hz 闪烁（偶数秒亮、奇数秒灭） */
    const state_indicator_state_t crit =
        make(true, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_CRITICAL, false);
    CHECK_U32(state_indicator_dot_mask(&crit, 0U), LCD_DOT_LOW_BAT);
    CHECK_U32(state_indicator_dot_mask(&crit, 1U), 0U);
    CHECK_U32(state_indicator_dot_mask(&crit, 2U), LCD_DOT_LOW_BAT);
    CHECK_U32(state_indicator_dot_mask(&crit, 3U), 0U);

    /* 电量未知：不点 DOT4 */
    const state_indicator_state_t unknown =
        make(true, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_UNKNOWN, false);
    CHECK_U32(state_indicator_dot_mask(&unknown, 0U), 0U);

    /* 故障：DOT5 亮、数字位清空；其余状态点照常（并发规则） */
    const state_indicator_state_t fault =
        make(true, CLOCK_RTC_TIME_TRUSTED, BATTERY_LEVEL_NORMAL, true);
    CHECK_U32(state_indicator_dot_mask(&fault, 0U), LCD_DOT_FAULT);
    CHECK(state_indicator_digits_blanked(&fault));

    const state_indicator_state_t fault_all =
        make(false, CLOCK_RTC_TIME_NEVER_SET, BATTERY_LEVEL_CRITICAL, true);
    CHECK_U32(state_indicator_dot_mask(&fault_all, 0U),
              LCD_DOT_NO_SEED | LCD_DOT_NEED_SYNC | LCD_DOT_LOW_BAT | LCD_DOT_FAULT);
    CHECK_U32(state_indicator_dot_mask(&fault_all, 1U),
              LCD_DOT_NO_SEED | LCD_DOT_NEED_SYNC | LCD_DOT_FAULT);
    CHECK(state_indicator_digits_blanked(&fault_all));

    /* NULL 防御：空状态返回空掩码并留空数字 */
    CHECK_U32(state_indicator_dot_mask(NULL, 0U), 0U);
    CHECK(state_indicator_digits_blanked(NULL));
}
