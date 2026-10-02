/**
 * @file    battery.h
 * @brief   CR2032 电量监测（内部 VREFINT 反推 VDD）。
 *
 * 原理：STM32L031 内置稳定带隙基准 VREFINT（典型 1.224 V，连到 ADC 通道 17），
 *       而 ADC 的参考就是 VDD 本身。因此测得 VREFINT 的 ADC 码值即可反推 VDD：
 *
 *           VDD = VREFINT_CAL * 3000mV / VREFINT_DATA
 *
 *       VREFINT_CAL 是出厂校准值，存于 0x1FF80078，校准条件为 25 °C、VDDA = 3.0 V。
 *
 * NOTE: 本板没有电池分压网络，PA0 也悬空，因此无法用外部采样 ——
 *       这个方案零外部元件，正好绕开该限制。
 *
 * NOTE: HT1621B 的 VLCD 直连 VDD 且无稳压，CR2032 从 3.0 V 放到 2.5 V 时
 *       对比度会明显变淡。因此低电告警必须**早于**对比度不可读发生，
 *       阈值需按本板屏体的实际可读下限标定。
 */
#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/** 电量档位，数值越大越危急 */
typedef enum {
    BATTERY_LEVEL_NORMAL  = 0,  /* VDD > 阈值上限，无需提示 */
    BATTERY_LEVEL_LOW     = 1,  /* 建议准备更换电池 */
    BATTERY_LEVEL_CRITICAL = 2, /* 对比度即将不可读，应立即更换 */
    BATTERY_LEVEL_UNKNOWN = 3,  /* 采样失败或尚未初始化 */
} battery_level_t;

/**
 * @brief  初始化 ADC 与 VREFINT（不启动转换）。
 * @note   调用后需等待至少 VREFINT 启动时间（约 2–3 µs）才能采样；
 *         battery_read_mv() 内部已包含该等待。
 */
void battery_init(void);

/**
 * @brief  单次测量 VDD，返回毫伏值。
 * @return VDD 毫伏值；若采样异常返回 0。
 * @note   单次测量含 VREFINT 启动、ADC 上电/校准/稳定与一次转换，
 *         总耗时约 250 µs（其中采样 + 转换 173 个 ADC 周期 ≈ 165 µs）；
 *         按每 10 分钟一次折算平均约 3 nA，对平均功耗的影响可忽略。
 */
uint32_t battery_read_mv(void);

/**
 * @brief  测量并用滑动平均滤波，返回稳定的 VDD 毫伏值。
 * @param  samples 参与平均的采样次数，建议 8（兼顾噪声抑制与耗时）。
 * @return 滤波后的 VDD 毫伏值。
 * @note   建议在周期性唤醒时调用；单次转换噪声较大，平均后常温精度约 ±30–50 mV。
 */
uint32_t battery_read_mv_filtered(uint8_t samples);

/**
 * @brief  依据当前电压判定电量档位。
 * @param  vdd_mv 由 battery_read_mv_filtered() 得到的电压值。
 * @return 电量档位。
 * @note   阈值为出厂默认（2.4 V / 2.2 V）；更换屏体或电池批次后重新
 *         标定：测出"对比度开始不可读"的实际电压后再定档。
 */
battery_level_t battery_classify(uint32_t vdd_mv);

#endif /* BATTERY_H */
