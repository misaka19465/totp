/**
 * @file    board_pins.h
 * @brief   硬件引脚与外部器件资源映射（板：mshv-checkin / Board3）。
 *
 * 本文件是固件与原理图的唯一对照点：任何引脚改动都必须同时更新此处与原理图。
 * 原理图依据：STM32L031F6P6 (TSSOP-20) + HT1621BMS/TR (MSKSEMI, SSOP-28) + EDS826 六位段码屏，
 * 网络名取自 EasyEDA 工程 mshv-checkin 的 Board3/Schematic3。
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "stm32l031xx.h"

/* ---- 指示用小数点（仅 4 个可用，见 docs/hardware.md） ---- */
/* NOTE: 段码屏 6 位，仅在位 1/2 上方与位 4/5 下方存在 4 个独立小数点，
 *       无第 3 位与第 6 位小数点。状态显示方案必须适配这 4 个点位。 */

/* ---- HT1621B 三线串行接口 ---- */
#define LCD_CS_PORT        GPIOA
#define LCD_CS_PIN         6U   /* PA6  -> U2 pin 6  CS#  */
#define LCD_WR_PORT        GPIOA
#define LCD_WR_PIN         3U   /* PA3  -> U2 pin 7  WR#  */
#define LCD_DATA_PORT      GPIOA
#define LCD_DATA_PIN       2U   /* PA2  -> U2 pin 8  DATA */

/* ---- 未连接引脚 ---- */
/* NOTE: PA0/PA1/PA4/PA5/PA7/PB1 均未连接。上电后必须显式配为模拟输入，
 *       浮空输入会使输入级振荡漏电（MCU 侧预算仅 1 µA，见 architecture.md §4.5）。 */
/* NOTE: 单行书写，避免多行宏漏写续行符。每脚占 MODER 的 2 bit，11 = 模拟。 */
#define PIN_ANALOG_MASK_GPIOA   ((3U << (0U * 2U)) | (3U << (1U * 2U)) | (3U << (4U * 2U)) | (3U << (5U * 2U)) | (3U << (7U * 2U)))
#define PIN_ANALOG_MASK_GPIOB   ((3U << (1U * 2U)))

/* ---- 耳机电平串口（对时/写种子入口，3.5 mm TRRS PJ-342） ---- */
#define UART_TX_PORT       GPIOA
#define UART_TX_PIN        9U   /* PA9  -> CN1 pin 3  TIP   */
#define UART_RX_PORT       GPIOA
#define UART_RX_PIN        10U  /* PA10 -> CN1 pin 6  RING1 */
/* NOTE: CN1 的 SLEEVE(5) 与 RING2(4) 均接 GND；pin 2、4、7 在原理图上为悬空(X)。
 *       因此可用触点仅 3 个：TIP=TX、RING1=RX、SLEEVE=GND。这对半双工/全双工
 *       协议设计有直接影响，详见 docs/serial-protocol.md。 */

/* ---- 调试接口（仅烧录，不承载产品功能） ---- */
/* J2: 1=VDD 2=GND 3=SWDIO(PA13) 4=SWCLK(PA14) */

/* ---- 时钟 ---- */
/* X1: EPSON FC-135 32.768 kHz 接 PC14/PC15 (LSE)，C4/C5 = 22 pF */

/* ---- 电量监测 ---- */
/* NOTE: 板上没有电池电压分压网络，PA0 也悬空，因此不能用外部采样。
 *       改用内部 VREFINT + ADC 通道 17 反推 VDD，零外部元件。
 *       详见 docs/hardware.md H5 与 firmware/src/drivers/battery.h。 */

/* ---- 电源 ---- */
/* BT1: CR2032 座，无 LDO，VDD 直连电池；HT1621B 的 VLCD 亦直接接 VDD。
 * NOTE: 无 VBAT 引脚、无备份电容 => 换电池必然丢失 RTC 时间，需重新对时。
 *       这是已接受的硬件限制，见 docs/requirements.md NFR-5。 */

#endif /* BOARD_PINS_H */
