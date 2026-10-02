/**
 * @file    clock_rtc.h
 * @brief   RTC 走时服务：LSE 启振、UTC 毫秒读写、对时状态与数字校准。
 *
 * 职责（architecture.md §3 services/clock_rtc）：
 *   - 上电时以 LSE 为 RTC 时钟源完成初始化；系统复位（含 IWDG）不打断走时；
 *   - 提供 UTC 毫秒读写：设备侧把毫秒量化到 1/256 s，并用 RTC 移位寄存器
 *     （RTC_SHIFTR，RM0367 §22.4.10）对齐亚秒相位 —— RTC_SSR 是只读的，
 *     不能直接写亚秒计数器（RM0367 §22.7.10）；
 *   - 在 RTC 备份寄存器记录"上次对时"，给出 serial-protocol.md §5.3 的
 *     TRUSTED / NEVER_SET / STALE 标志。备份寄存器无 VBAT 供电：
 *     换电池（RTC 域复位）即失，与 RTC 本身同命运；
 *   - 提供 SET_CALIB 的 0.01 ppm 折算与回读（serial-protocol.md §5.8）。
 *
 * NOTE: 本模块不做低功耗管理，由调用方（主循环/电源状态机）决定调用时机；
 *       单次读 <1 ms；单次写约 10–20 ms（主要是 SHIFTR 生效等待 ~7.8 ms），
 *       SET_TIME 端到端响应实测 <50 ms，满足协议 §7 的 ≤100 ms 期限。
 */
#ifndef CLOCK_RTC_H
#define CLOCK_RTC_H

#include <stdbool.h>
#include <stdint.h>

/* time_flags 位定义，与 serial-protocol.md §5.3 一一对应 */
#define CLOCK_RTC_TIME_TRUSTED   (1U << 0) /**< 已对时，走时可信 */
#define CLOCK_RTC_TIME_NEVER_SET (1U << 1) /**< 本电池周期内从未对时 */
#define CLOCK_RTC_TIME_STALE     (1U << 2) /**< 距上次对时超过阈值 */

/** STALE 判定阈值：距上次对时 30 天（serial-protocol.md §5.4） */
#define CLOCK_RTC_STALE_AFTER_S (30U * 24U * 3600U)

/* SET_TIME 设备侧自检容差（ms），与 §5.4 上位机判据同值。
 * NOTE: 自检只用于识别"写错整秒/写错日历"级别的错误，不承诺毫秒级精度：
 *       回读发生在移位生效之后，且链路单程时延随 USB 转串口链路波动
 *       （实机 2026-10-02：回读 − 发送 ≈ +13 ms，恒定偏置 ≈ −58 ms）。
 *       100 ms 对 30 s 的 TOTP 窗口仍有 300 倍以上余量。 */
#define CLOCK_RTC_SET_TOLERANCE_MS 100U

/**
 * @brief  使能 LSE 并初始化 RTC（可重复调用）。
 * @return LSE 就绪且日历可用返回 true；LSE 起振超时（3 s）返回 false。
 * @note   若 RTC 已在 LSE 下走时（IWDG/软件复位后），保留当前时间直接返回；
 *         否则配置 1 Hz + 1/256 s 亚秒并写入默认日历 2001-01-01T00:00:00Z，
 *         同时保持"从未对时"状态（NEVER_SET）。换电池后调用会经历 LSE
 *         冷启动（典型 <1 s，最坏 ~2 s），其余情况不阻塞。
 *         超时返回 false 后 LSE 仍保持使能，下次调用可重试。
 */
bool clock_rtc_init(void);

/** RTC 是否已就绪（clock_rtc_init 成功后为 true，直到复位）。 */
bool clock_rtc_ready(void);

/**
 * @brief  读取当前 UTC 时间（毫秒）。
 * @return 当前 Unix 毫秒；RTC 未就绪或读取校验失败返回 0。
 * @note   亚秒按 1/256 s 量化：读回值可能比真实时间小最多 3.9 ms。
 */
uint64_t clock_rtc_now_ms(void);

/**
 * @brief  时间可信标志（CLOCK_RTC_TIME_* 的按位或）。
 * @return 组合标志；从未对时返回 CLOCK_RTC_TIME_NEVER_SET，
 *         超期或读取失败返回 CLOCK_RTC_TIME_STALE。
 */
uint8_t clock_rtc_time_flags(void);

/**
 * @brief  距上次对时的秒数。
 * @return 秒数；从未对时返回 UINT32_MAX，读取失败或时钟回退返回 0。
 */
uint32_t clock_rtc_last_set_age_s(void);

/**
 * @brief  写入 UTC 毫秒时间并回读校验（SET_TIME，serial-protocol.md §5.4）。
 * @param  unix_ms     目标时间；仅支持 2000..2099 年（RTC 年字段限制）。
 * @param  readback_ms 输出设备回读值；可为 NULL。
 * @return 写入后回读误差 ≤ CLOCK_RTC_SET_TOLERANCE_MS 返回 true，
 *         此时"上次对时"更新为 unix_ms；否则返回 false 且不改变对时状态。
 * @note   写入路径实测约 10–20 ms（SHIFTR 生效等待 ~7.8 ms 为主），
 *         SET_TIME 端到端响应 <50 ms（含 115200 链路往返）。
 */
bool clock_rtc_set_unix_ms(uint64_t unix_ms, uint64_t *readback_ms);

/**
 * @brief  写 RTC 数字校准（SET_CALIB，serial-protocol.md §5.8）。
 * @param  centi_ppm         请求修正量，0.01 ppm 单位，范围 ±30000；0 = 关闭。
 * @param  applied_centi_ppm 输出回读到的实际生效值；可为 NULL。
 * @return 写入并回读成功返回 true；参数超范围或 RTC 未就绪返回 false。
 * @note   量化到 0.9537 ppm 一档，±300 ppm 内误差 ≤ 0.48 ppm。
 */
bool clock_rtc_set_calib(int16_t centi_ppm, int16_t *applied_centi_ppm);

/** 是否启用了数字校准（GET_STATUS flags 的 CALIB_ACTIVE 位用）。 */
bool clock_rtc_calib_active(void);

/**
 * @brief  武装 RTC 闹钟 A（单次、整秒时:分:秒匹配，日期不参与）。
 * @param  unix_ms 目标时刻（UTC 毫秒）；实际触发点取其所在整秒，若该整秒
 *                 此刻已到/已过，则顺延到下一秒。
 * @return 写入成功返回 true；RTC 未就绪、时间超出 2000..2099 或硬件超时
 *         返回 false（失败时闹钟保持停用，不会残留半套配置）。
 * @note   只适用于 24 h 以内的期限（主循环最长为 10 min 电量采集）。
 *         触发路径：RTC 闹钟 → EXTI 17 → NVIC RTC_IRQn；RTC_IRQHandler
 *         清标志并停用闹钟，主循环每次入睡前重新武装。
 */
bool clock_rtc_alarm_set(uint64_t unix_ms);

#endif /* CLOCK_RTC_H */
