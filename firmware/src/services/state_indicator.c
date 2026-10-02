/**
 * @file    state_indicator.c
 * @brief   状态点策略实现（纯计算，见 state_indicator.h）。
 */
#include "state_indicator.h"

#include <stddef.h>

#include "clock_rtc.h"
#include "lcd_render.h"

uint8_t state_indicator_dot_mask(const state_indicator_state_t *state,
                                 uint32_t unix_seconds)
{
    if (state == NULL) {
        return 0U;
    }

    uint8_t mask = 0U;

    if (!state->seed_present) {
        mask |= LCD_DOT_NO_SEED;
    }
    if ((state->time_flags & (CLOCK_RTC_TIME_NEVER_SET | CLOCK_RTC_TIME_STALE)) != 0U) {
        mask |= LCD_DOT_NEED_SYNC;
    }

    switch (state->battery_level) {
    case BATTERY_LEVEL_NORMAL:
    case BATTERY_LEVEL_UNKNOWN:
        break;
    case BATTERY_LEVEL_LOW:
        mask |= LCD_DOT_LOW_BAT;
        break;
    case BATTERY_LEVEL_CRITICAL:
        /* NOTE: 极低档闪烁 = 1 Hz、50% 占空比；主循环在极低档把显示重写
         *       间隔缩短到 1 s，保证该相位真实翻转（§4.1）。 */
        if ((unix_seconds % 2U) == 0U) {
            mask |= LCD_DOT_LOW_BAT;
        }
        break;
    }

    if (state->fault_latched) {
        mask |= LCD_DOT_FAULT;
    }
    return mask;
}

bool state_indicator_digits_blanked(const state_indicator_state_t *state)
{
    if (state == NULL) {
        return true;
    }
    return state->fault_latched || !state->seed_present;
}
