/**
 * @file    fake_clock_rtc.h
 * @brief   主机侧测试替身：用可控状态替代 RTC 服务（clock_rtc.h 的 API）。
 *
 * proto 的响应字节依赖时间、对时状态与校准读数，测试需要逐项控制这些量，
 * 并观察 SET_TIME/SET_CALIB 实际写入的参数。
 */
#ifndef FAKE_CLOCK_RTC_H
#define FAKE_CLOCK_RTC_H

#include <stdbool.h>
#include <stdint.h>

/** 复位为默认状态：ready、now=0、flags=0、校准未启用、写入均成功 */
void fake_clock_rtc_reset(void);

void fake_clock_rtc_set_ready(bool ready);
void fake_clock_rtc_set_now_ms(uint64_t ms);
void fake_clock_rtc_set_time_flags(uint8_t flags);
void fake_clock_rtc_set_last_set_age_s(uint32_t age_s);
void fake_clock_rtc_set_calib_active(bool active);

/** 让下一次 clock_rtc_set_unix_ms / clock_rtc_set_calib 返回失败 */
void fake_clock_rtc_fail_set_time(bool fail);
void fake_clock_rtc_fail_set_calib(bool fail);

/** 最近一次成功调用传入的参数（未调用过为 0） */
uint64_t fake_clock_rtc_last_set_time_arg(void);
int16_t fake_clock_rtc_last_calib_arg(void);

#endif /* FAKE_CLOCK_RTC_H */
