/**
 * @file    state_indicator.h
 * @brief   状态点策略：由设备状态推导 4 个小数点的掩码与数字位留空规则。
 *
 * 语义与并发规则：四个点互不重叠、可同时点亮；
 * 唯一的覆盖规则是"故障清空数字位"（显示 ------），避免参与者记录无效码。
 * 本模块只做策略计算、不访问硬件；渲染与落屏属 drivers/lcd_render。
 */
#ifndef STATE_INDICATOR_H
#define STATE_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "battery.h"

/** 状态点策略的输入快照（由主循环从各服务收集） */
typedef struct {
    bool            seed_present;  /**< keystore 中是否有有效配置 */
    uint8_t         time_flags;    /**< CLOCK_RTC_TIME_* 位掩码   */
    battery_level_t battery_level; /**< 最近一次滤波后的电量档位  */
    bool            fault_latched; /**< 故障锁存（含 IWDG 复位）  */
} state_indicator_state_t;

/**
 * @brief  计算状态点掩码。
 * @param  state         状态快照；不得为 NULL。
 * @param  unix_seconds  当前 UTC 整秒（极低电量闪烁的相位依据）。
 * @return LCD_DOT_* 的按位或；极低电量按 1 Hz 给出亮/灭相位。
 */
uint8_t state_indicator_dot_mask(const state_indicator_state_t *state,
                                 uint32_t unix_seconds);

/**
 * @brief  数字位是否应留空。
 * @param  state 状态快照；不得为 NULL。
 * @return 故障锁存、时间不可信（NEVER_SET/STALE）或未写入种子时返回 true
 *         （当前没有可信的码可显示）。
 */
bool state_indicator_digits_blanked(const state_indicator_state_t *state);

#endif /* STATE_INDICATOR_H */
