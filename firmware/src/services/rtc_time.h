/**
 * @file    rtc_time.h
 * @brief   RTC 日历与 UTC 时间的纯换算（不访问寄存器，可主机侧测试）。
 *
 * 约定（与 STM32L0 RTC 对齐）：
 *   - 日历为公历 UTC；RTC_DR 年字段是 00..99 的偏移，故仅支持 2000..2099；
 *   - 亚秒粒度 1/256 s（PREDIV_S = 255），本模块用"自整秒边界起的计数"
 *     表示亚秒：ticks = 255 - SSR，0..255 表示整秒内已过的 1/256 s；
 *   - 周几沿用 RTC_DR.WDU：1 = 周一 … 7 = 周日。
 *
 * NOTE: 本模块只做数学换算，不判断"时间是否可信"（TRUSTED/STALE 由
 *       services/clock_rtc 依据备份寄存器维护）。
 */
#ifndef RTC_TIME_H
#define RTC_TIME_H

#include <stdbool.h>
#include <stdint.h>

/** RTC 年字段能表达的最早/最晚年份 */
#define RTC_TIME_YEAR_MIN 2000U
#define RTC_TIME_YEAR_MAX 2099U

/** 亚秒计数上限：255 表示整秒内最后一个 1/256 s */
#define RTC_TIME_TICKS_MAX 255U

/** 拆分后的公历 UTC 时刻 */
typedef struct {
    uint16_t year;    /**< 2000..2099 */
    uint8_t  month;   /**< 1..12 */
    uint8_t  day;     /**< 1..31，按月份与闰年校验 */
    uint8_t  weekday; /**< 1..7（1 = 周一） */
    uint8_t  hour;    /**< 0..23 */
    uint8_t  minute;  /**< 0..59 */
    uint8_t  second;  /**< 0..59 */
} rtc_time_civil_t;

/** RTC 平滑校准寄存器字段（CALP + CALM[8:0]，RM0367 §22.4.12） */
typedef struct {
    bool     plus;  /**< CALP：每 2^20 个 RTCCLK 插入 512 个脉冲（+488.28125 ppm） */
    uint16_t minus; /**< CALM[8:0]：每 2^20 个 RTCCLK 屏蔽 0..511 个脉冲（每档 0.9537 ppm） */
} rtc_time_calib_t;

/**
 * @brief  RTC 寄存器用的 BCD 字节 → 二进制。
 * @param  bcd     待转换值（高低半字节都必须 ≤ 9）。
 * @param  bin_out 输出 0..99；可为 NULL，仅校验。
 * @return 合法返回 true；出现 A..F 半字节返回 false（不修改 *bin_out）。
 */
bool rtc_time_bcd_to_bin(uint8_t bcd, uint8_t *bin_out);

/**
 * @brief  二进制（0..99）→ BCD 字节。
 * @param  bin 0..99（调用方保证；>99 时结果无定义）。
 */
uint8_t rtc_time_bin_to_bcd(uint8_t bin);

/**
 * @brief  UTC 整秒 → 公历时刻（含周几）。
 * @param  unix_s  1970-01-01 起的秒数。
 * @param  out     输出的日历时刻。
 * @return 年份落在 RTC_TIME_YEAR_MIN..RTC_TIME_YEAR_MAX 且 out 非空返回 true。
 */
bool rtc_time_from_unix(uint64_t unix_s, rtc_time_civil_t *out);

/**
 * @brief  公历时刻 → UTC 整秒。
 * @param  civil       输入时刻；字段越界（含非闰年 2 月 29 日、周几不在 1..7）返回 false。
 * @param  unix_s_out  输出整秒。
 * @return 合法返回 true；否则 false（不修改 *unix_s_out）。
 * @note   weekday 冗余于日期，仅校验范围、不校验与日期是否自洽；
 *         设备侧由 rtc_time_from_unix() 计算后再写寄存器。
 */
bool rtc_time_to_unix(const rtc_time_civil_t *civil, uint64_t *unix_s_out);

/**
 * @brief  毫秒（0..999）→ 亚秒计数（0..255，向下取整）。
 * @param  ms         整秒内的毫秒数，必须 < 1000。
 * @param  ticks_out  输出 0..255。
 * @return 参数合法返回 true；否则 false（不修改 *ticks_out）。
 */
bool rtc_time_ticks_from_ms(uint16_t ms, uint8_t *ticks_out);

/**
 * @brief  亚秒计数 → 毫秒（向下取整）。
 * @param  ticks 自整秒边界起的 1/256 s 计数；RTC 移位后可为负
 *               （此时读数含义是"比日历秒字段早 |ticks|/256 s"，RM0367 §22.7.10）。
 * @return 毫秒数，范围 -1000..996。
 */
int32_t rtc_time_ms_from_ticks(int32_t ticks);

/**
 * @brief  0.01 ppm 校准请求 → 最接近的 CALP/CALM 档位。
 * @param  centi_ppm 请求修正量，范围 ±30000（±300 ppm）。
 * @param  out       输出寄存器档位。
 * @return 合法返回 true；out 为空或超范围返回 false。
 * @note   档位为 0.9537 ppm 一步、覆盖 -487.4..+488.3 ppm；
 *         ±300 ppm 内的最大量化误差约 0.48 ppm（= 半档）。
 */
bool rtc_time_calib_from_centi_ppm(int32_t centi_ppm, rtc_time_calib_t *out);

/**
 * @brief  CALP/CALM 档位 → 实际修正量（0.01 ppm，四舍五入）。
 * @param  calib 寄存器档位；minus 超出 0..511 按原值计算（仅用于回读校验）。
 * @return 修正量，范围约 -48742..+48828（0.01 ppm）。
 */
int32_t rtc_time_calib_to_centi_ppm(const rtc_time_calib_t *calib);

#endif /* RTC_TIME_H */
