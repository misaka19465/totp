/**
 * @file    main.c
 * @brief   上电入口与低功耗主循环（唯一了解全局的模块）。
 *
 * 职责（docs/architecture.md §3/§4）：
 *   - 启动顺序：时钟/引脚 → 屏 → 电源档位 → RTC → 看门狗 → 串口链路 → 电量；
 *   - 空闲时进入 Stop（RTC 闹钟 + PA10 EXTI 唤醒），按三类期限调度：
 *     口令窗口边界（精确）、显示重写（2 s，极低电量时 1 s）、电量采集（10 min）；
 *   - 串口会话期间保持清醒，把请求交给 app/proto，成功后立即刷新显示；
 *   - 收集 keystore/RTC/电量/看门狗状态，交给 state_indicator 决定显示。
 *
 * NOTE: 屏由 HT1621B 自主驱动，MCU 睡觉不影响常亮；显示重写是为了缓解
 *       直流偏置累积（§4.3），不是保持内容所必需。
 */
#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "board_pins.h"
#include "clock_rtc.h"
#include "ht1621.h"
#include "keystore.h"
#include "lcd_render.h"
#include "power.h"
#include "proto.h"
#include "state_indicator.h"
#include "stm32l031xx.h"
#include "totp.h"
#include "uart_link.h"
#include "watchdog.h"

/* 显示重写间隔：1–2 s 的规格取上限，功耗减半（architecture.md §4.3/§4.4） */
#define MAIN_DISPLAY_REFRESH_MS   2000U
/* 极低电量闪烁需要 1 Hz 相位翻转，刷新间隔必须到 1 s（requirements.md §4.1） */
#define MAIN_BLINK_REFRESH_MS     1000U
/* 电量采集间隔（architecture.md §4.4 闹钟 C） */
#define MAIN_BATTERY_INTERVAL_MS  600000U

/* ---- 运行时状态 ---- */

static keystore_config_t s_cfg;                  /* 当前点位配置（缓存） */
static bool     s_cfg_present;                   /* keystore 中是否有有效记录 */
static uint32_t s_code;                          /* 当前窗口的口令 */
static uint64_t s_code_end_ms;                   /* 当前口令有效的窗口边界 */
static bool     s_code_valid;                    /* s_code 是否可用 */
static bool     s_display_dirty = true;          /* 需要立即重绘 */
static uint64_t s_flush_due_ms;                  /* 下次显示重写期限 */
static uint64_t s_battery_due_ms;                /* 下次电量采集期限 */
static uint64_t s_boot_ms;                       /* 上电时刻（uptime 基准） */
static uint32_t s_battery_mv;                    /* 最近一次滤波电压（mV） */
static battery_level_t s_battery_level = BATTERY_LEVEL_UNKNOWN;

/* ---- 时钟与引脚（沿用骨架） ---- */

static void clock_init(void)
{
    /* 复位后默认时钟为 MSI 2.097 MHz，此处不升频：
       本设备绝大多数时间处于 Stop 模式，跑得越快越费电。 */
    SystemCoreClockUpdate();
}

static void gpio_init(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;

    /* NOTE: 所有未连接引脚显式置为模拟输入（MODER = 11）。
     *       浮空输入会使输入级振荡漏电，而 MCU 侧功耗预算只有 1 µA；
     *       见 docs/architecture.md §4.5 与 board_pins.h 的说明。
     *       模拟态同时也是这些引脚复位后的默认值，此处只是写死以防误改。 */
    GPIOA->MODER |= PIN_ANALOG_MASK_GPIOA;
    RCC->IOPENR |= RCC_IOPENR_GPIOBEN;
    GPIOB->MODER |= PIN_ANALOG_MASK_GPIOB;
}

/* ---- 降级时基（RTC 不可用时支撑串口会话超时） ---- */

/* NOTE: LSE 起振失败时 clock_rtc_now_ms() 恒为 0，uart_link 的 300 ms 唤醒
 *       窗口与 10 s 会话超时会失效（停留 WAKE 不睡，拔出插头也耗电）。
 *       SysTick 只在清醒期自由运行、Stop 下暂停，正好覆盖会话期间的计时；
 *       本降级路径按每次调用最多推进 1 ms 计，长操作（EEPROM 写）期间的
 *       丢拍只会让超时略偏长，不影响正确性。 */
static bool     s_systick_running;
static uint32_t s_fallback_ms;

static uint32_t fallback_ms_now(void)
{
    if (!s_systick_running) {
        uint32_t reload = SystemCoreClock / 1000U;
        if (reload == 0U) {
            reload = 1U;
        }
        SysTick->LOAD = reload - 1U;
        SysTick->VAL = 0U;
        SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
        (void)SysTick->CTRL;   /* 读一次清掉遗留的 COUNTFLAG */
        s_systick_running = true;
    }
    if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U) {
        s_fallback_ms++;
    }
    return s_fallback_ms;
}

/** 当前时刻（UTC 毫秒）；RTC 不可用时退化为毫秒计数（仅会话计时用）。 */
static uint64_t app_now_ms(void)
{
    if (clock_rtc_ready()) {
        return clock_rtc_now_ms();
    }
    return (uint64_t)fallback_ms_now();
}

/** 显示重写间隔：极低电量用 1 s 让闪烁相位可见，其余 2 s。 */
static uint32_t display_refresh_interval_ms(void)
{
    return (s_battery_level == BATTERY_LEVEL_CRITICAL) ? MAIN_BLINK_REFRESH_MS
                                                       : MAIN_DISPLAY_REFRESH_MS;
}

/* ---- 状态收集与显示 ---- */

static void config_reload(void)
{
    s_cfg_present = keystore_load(&s_cfg);
    s_code_valid = false;
}

/** 采集电量：失败（返回 0）时保留上一次的有效值。 */
static void battery_sample(void)
{
    const uint32_t mv = battery_read_mv_filtered(8U);
    if (mv != 0U) {
        s_battery_mv = mv;
        s_battery_level = battery_classify(mv);
    }
}

static state_indicator_state_t indicator_state(void)
{
    state_indicator_state_t state;
    state.seed_present = s_cfg_present;
    state.time_flags = clock_rtc_time_flags();
    state.battery_level = s_battery_level;
    state.fault_latched = watchdog_fault_latched();
    return state;
}

static void display_refresh(uint64_t now_ms)
{
    const state_indicator_state_t state = indicator_state();
    const uint8_t dots =
        state_indicator_dot_mask(&state, (uint32_t)(now_ms / 1000U));

    uint32_t code = 0U;
    uint8_t digits = 0U;
    if (!state_indicator_digits_blanked(&state) && s_code_valid) {
        code = s_code;
        digits = s_cfg.digits;
    }
    (void)lcd_render_show(code, digits, dots);
}

/**
 * @brief  按需重算当前窗口的口令。
 * @return 口令发生变化（需要重绘）返回 true。
 * @note   窗口未跨越时不重算（HMAC-SHA1 只在边界执行一次）；
 *         计算失败说明配置或算法状态异常，锁存故障并留空数字位。
 */
static bool code_update(uint64_t now_ms)
{
    if (!s_cfg_present || !clock_rtc_ready() || now_ms == 0U) {
        s_code_valid = false;
        return false;
    }

    const uint64_t now_s = now_ms / 1000U;
    if (s_code_valid && now_s < (s_code_end_ms / 1000U)) {
        return false;
    }

    if (!totp_generate(s_cfg.secret, s_cfg.secret_len, now_s, s_cfg.period_s,
                       s_cfg.digits, &s_code)) {
        s_code_valid = false;
        watchdog_latch_fault();
        return true;
    }
    s_code_valid = true;
    s_code_end_ms = totp_next_boundary_ms(now_ms, s_cfg.period_s);
    return true;
}

/* ---- 串口会话 ---- */

static proto_status_t status_build(uint64_t now_ms)
{
    proto_status_t status;
    /* 对时可能把时钟拨回：回拨期间 uptime 归零而不是回绕成天文数字 */
    const uint64_t uptime_ms = (now_ms >= s_boot_ms) ? (now_ms - s_boot_ms) : 0U;
    status.uptime_s = (uint32_t)(uptime_ms / 1000U);
    status.reset_cause = (uint8_t)watchdog_reset_cause();
    status.battery_mv = (uint16_t)s_battery_mv;
    status.battery_level = s_battery_level;
    status.fault_latched = watchdog_fault_latched();
    status.iwdg_enabled = watchdog_enabled();
    return status;
}

/**
 * @brief  处理一帧串口请求，并在配置/时间变化后立即刷新显示（NFR-3）。
 * @note   成功的 SET_TIME / WRITE_SLOT 同时清除故障锁存（requirements.md §4.0）。
 */
static void serial_handle(uint64_t now_ms)
{
    uart_link_frame_t frame;
    if (!uart_link_poll((uint32_t)now_ms, &frame)) {
        return;
    }

    const proto_status_t status = status_build(now_ms);
    proto_result_t result = PROTO_ERR_INTERNAL;
    if (!proto_handle_request(&frame, &status, &result)) {
        return;   /* 链路已休眠，响应未发出 */
    }
    if (result != PROTO_OK) {
        return;
    }

    if (frame.cmd == PROTO_CMD_WRITE_SLOT || frame.cmd == PROTO_CMD_ERASE_SLOT) {
        config_reload();
    }
    if (frame.cmd == PROTO_CMD_SET_TIME) {
        s_code_valid = false;   /* 时间可能大幅跳变，强制重算 */
    }
    if (frame.cmd == PROTO_CMD_SET_TIME || frame.cmd == PROTO_CMD_WRITE_SLOT) {
        watchdog_clear_fault();
    }
    s_display_dirty = true;
}

/* ---- 主循环 ---- */

int main(void)
{
    clock_init();
    gpio_init();
    ht1621_init();
    power_init();

    /* 时间服务：换电池后经历一次 LSE 冷启动（典型 <1 s）；
     * 系统复位（含 IWDG）时保留走时，直接返回。 */
    const bool rtc_ok = clock_rtc_init();

    watchdog_init();
    if (rtc_ok) {
        watchdog_start();
    } else {
        /* LSE 起振失败：时间不可用、码不可信；不启动 IWDG（无闹钟时
         * 无限期 Stop 会形成复位循环，见 watchdog.h 的说明）。 */
        watchdog_latch_fault();
    }

    uart_link_init();
    battery_init();

    config_reload();
    battery_sample();

    uint64_t now_ms = app_now_ms();
    s_boot_ms = now_ms;
    (void)code_update(now_ms);
    display_refresh(now_ms);
    s_flush_due_ms = now_ms + display_refresh_interval_ms();
    s_battery_due_ms = now_ms + MAIN_BATTERY_INTERVAL_MS;

    for (;;) {
        watchdog_feed();
        now_ms = app_now_ms();

        serial_handle(now_ms);

        if (code_update(now_ms)) {
            s_display_dirty = true;
        }
        /* NOTE: 会话期间跳过周期性重写：位翻转落屏要 1–3 ms，可能吞掉
         *       正在收的帧（USART 无 DMA/中断接收）。命令触发的刷新与
         *       窗口边界换码仍会立即落屏（NFR-3），配置期间最多 10 s
         *       不做直流偏置缓解，对月尺度的老化无影响。 */
        const bool link_idle = (uart_link_state() == UART_LINK_SLEEP);
        if (s_display_dirty || (link_idle && now_ms >= s_flush_due_ms)) {
            display_refresh(now_ms);
            s_display_dirty = false;
            s_flush_due_ms = now_ms + display_refresh_interval_ms();
        }
        if (now_ms >= s_battery_due_ms) {
            battery_sample();
            s_battery_due_ms = now_ms + MAIN_BATTERY_INTERVAL_MS;
            s_display_dirty = true;
        }

        if (uart_link_state() == UART_LINK_SLEEP) {
            /* NOTE: 入睡前喂狗：IWDG 在 Stop 下继续计数，本次睡眠上限
             *       由 2 s 的显示重写期限保证（最坏 LSI 下超时约 2.6 s）。 */
            watchdog_feed();
            if (!rtc_ok) {
                power_enter_stop();   /* 仅插头 EXTI 可唤醒 */
            } else if (now_ms != 0U) {
                uint64_t due_ms = s_flush_due_ms;
                if (s_code_valid && s_code_end_ms < due_ms) {
                    due_ms = s_code_end_ms;
                }
                if (s_battery_due_ms < due_ms) {
                    due_ms = s_battery_due_ms;
                }
                if (clock_rtc_alarm_set(due_ms)) {
                    power_enter_stop();
                }
                /* 武装失败（RTC 读写异常）：不睡，保持喂狗并重试 */
            }
            /* rtc_ok 但时钟读数为 0：RTC 异常，不睡，由看门狗兜底 */
        }
    }
}
