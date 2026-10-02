/**
 * @file    watchdog.h
 * @brief   IWDG 看门狗与故障锁存（无按键设备唯一的自恢复手段）。
 *
 * 设计依据 docs/architecture.md §7：IWDG 由 LSI 驱动、Stop 下继续计数，
 * 4 s 超时先作实测起点；一次完整唤醒周期在数 ms 量级，余量充足。
 *
 * 故障锁存语义（requirements.md §4.0）：
 *   - IWDG 复位（RCC_CSR.IWDGRSTF）或主循环自检失败 → 锁存故障；
 *   - 锁存值存于 RTC 备份寄存器 BKP2R（VDD 在即保留，换电池即清），
 *     因此能在 IWDG 复位后继续显示 DOT5 与空数字位；
 *   - 只有成功的 SET_TIME / WRITE_SLOT（proto 层确认）才由主循环清除。
 *
 * NOTE: 复位原因需要先于 RCC_CSR.RMVF 清除读取一次；watchdog_init() 完成
 *       捕获并把复位标志清掉，使下次复位的原因可信。BKP2R 访问依赖
 *       clock_rtc_init() 打开的 PWR_CR.DBP，因此本模块必须在 RTC 初始化
 *       之后调用（见 main.c 的启动顺序）。
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* NOTE: 取值与 serial-protocol.md §5.5 的 reset_cause 线上口径一致，
 *       主循环直接转换后填入 proto_status_t，无需再次映射。 */
typedef enum {
    WATCHDOG_RESET_POR      = 0, /**< 上电/欠压复位（POR/BOR） */
    WATCHDOG_RESET_IWDG     = 1, /**< 独立看门狗复位           */
    WATCHDOG_RESET_SOFTWARE = 2, /**< 软件复位                 */
    WATCHDOG_RESET_OTHER    = 3, /**< 引脚/选项字节等其它复位  */
    WATCHDOG_RESET_UNKNOWN  = 4, /**< 复位原因寄存器无有效标志 */
} watchdog_reset_cause_t;

/**
 * @brief  按可处置性优先级归并复位原因标志（纯逻辑，主机侧可测）。
 * @param  iwdg     独立看门狗复位标志置位。
 * @param  software 软件复位标志置位。
 * @param  power    电源类复位（POR/BOR）标志置位。
 * @param  other    引脚/选项字节/防火墙/WWDG 等其它标志任一置位。
 * @return 归并后的复位原因；全部为假时返回 UNKNOWN。
 * @note   多个标志可能同时置位（如上电瞬间带 PINRSTF）：IWDG 最需要现场
 *         处置，其后依次为软件、电源类、其它。
 */
static inline watchdog_reset_cause_t watchdog_classify_reset(
    bool iwdg, bool software, bool power, bool other)
{
    if (iwdg) {
        return WATCHDOG_RESET_IWDG;
    }
    if (software) {
        return WATCHDOG_RESET_SOFTWARE;
    }
    if (power) {
        return WATCHDOG_RESET_POR;
    }
    if (other) {
        return WATCHDOG_RESET_OTHER;
    }
    return WATCHDOG_RESET_UNKNOWN;
}

/**
 * @brief  读取并归并复位原因，恢复故障锁存，随后清除 RCC_CSR 复位标志。
 * @note   必须在 clock_rtc_init() 之后调用（见文件头 NOTE）；只做一次性
 *         登记，不启动 IWDG——启动时机由主循环按 RTC 可用性决定。
 */
void watchdog_init(void);

/**
 * @brief  启动 IWDG（LSI，约 4 s 超时；重复调用无副作用）。
 * @note   启动后不可关闭。主循环只在 RTC 走时可用时启动：RTC 不可用时
 *         唯一唤醒源是插头 EXTI，无限期 Stop 会周期性触发 IWDG 复位，
 *         形成复位循环（该降级路径的取舍见 architecture.md §7.4）。
 *         此外启动必须排在 clock_rtc_init() 之后：LSE 冷启动最长等待
 *         3 s，可能超过 IWDG 最坏超时（2.6 s）。
 */
void watchdog_start(void);

/** 喂狗；未启动时为空操作。 */
void watchdog_feed(void);

/** IWDG 是否已启动（GET_STATUS 的 IWDG_ENABLED 位）。 */
bool watchdog_enabled(void);

/** 本次上电的复位原因（watchdog_init 捕获的归并结果）。 */
watchdog_reset_cause_t watchdog_reset_cause(void);

/** 故障是否处于锁存状态（DOT5 / FAULT_LATCHED）。 */
bool watchdog_fault_latched(void);

/** 锁存故障（自检失败等非复位型故障）。 */
void watchdog_latch_fault(void);

/** 清除故障锁存（成功的 SET_TIME / WRITE_SLOT 后由主循环调用）。 */
void watchdog_clear_fault(void);

#endif /* WATCHDOG_H */
