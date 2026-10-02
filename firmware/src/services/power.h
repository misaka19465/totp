/**
 * @file    power.h
 * @brief   低功耗管理：Stop 模式进出（唤醒源由 RTC 闹钟与 PA10 EXTI 提供）。
 *
 * 设计要点：屏由 HT1621B 自主驱动，MCU 无事时进
 * Stop；唤醒源有三类——RTC 闹钟（口令窗口边界/显示重写/电量采集）、
 * PA10 下降沿 EXTI（插头插入，由 plug_detect 配置）。
 *
 * NOTE: 本模块只负责电源模式本身。各外设的开关归属各自的驱动：
 *       USART/HSI16 由 uart_link_sleep() 关闭，ADC 由 battery 驱动测完即断，
 *       PVD 默认关闭。SWD 调试单元默认 DBGMCU_CR.DBG_STOP=0，
 *       Stop 下随内核断电，仅调试器连接时才会被置 1（仅开发场景）。
 */
#ifndef POWER_H
#define POWER_H

/**
 * @brief  配置 Stop 模式的低功耗档位（可重复调用）。
 * @note   置 PWR_CR.LPSDSR（稳压器低功耗）、清 PDDS（WFI 进 Stop 而非
 *         Standby）、清 PVDE（PVD 常开电流超过 MCU Stop 本身）。
 */
void power_init(void);

/**
 * @brief  进入 Stop 模式等待中断，函数返回即已唤醒。
 * @note   调用方必须保证：链路处于 SLEEP（USART/HSI16 已关）、无未处理
 *         的挂起事件，且已按需武装 RTC 闹钟；否则会立即返回或睡过头。
 *         调用前应先喂狗：IWDG 在 Stop 下继续计数。
 */
void power_enter_stop(void);

#endif /* POWER_H */
