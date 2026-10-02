/**
 * @file    main.c
 * @brief   上电入口与主循环（骨架，功能待实现）。
 *
 * 目标：CR2032 供电、常亮 6 位段码的打卡点动态码牌；无任何按键、非安全设备。
 * NOTE: 本文件在 initial commit 阶段只做最小可运行验证（点亮显示 + 采样电量），
 *       对时/写种子/低功耗状态机等能力见 docs/architecture.md 的落地顺序。
 */
#include "battery.h"
#include "board_pins.h"
#include "ht1621.h"
#include "stm32l031xx.h"

/** 点亮全部 12 个 SEG 的 4 个 COM（自检图案：6 位全 8 且 4 个小数点全亮） */
static void lcd_self_test(void)
{
    static const uint8_t pattern[6] = {0x0FU, 0x0FU, 0x0FU, 0x0FU, 0x0FU, 0x0FU};
    ht1621_write(HT1621_ADDR_DIGIT123, pattern, 6U);   /* 地址 0x00..0x05 */
    ht1621_write(HT1621_ADDR_DIGIT456, pattern, 6U);   /* 地址 0x0D..0x12 */
}

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

int main(void)
{
    clock_init();
    gpio_init();

    ht1621_init();
    lcd_self_test();

    /* TODO: 电量阈值上报 DOT4（FR-7 / §4.1）在状态机落地后接入 */
    battery_init();
    (void)battery_read_mv_filtered(1U);

    for (;;) {
        /* TODO: 主循环/低功耗状态机在本阶段之后实现，见 docs/architecture.md */
        __asm volatile ("wfi");
    }
}
