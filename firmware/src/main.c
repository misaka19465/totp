/**
 * @file    main.c
 * @brief   上电入口与主循环（骨架，低功耗状态机待实现）。
 *
 * 目标：CR2032 供电、常亮 6 位段码的打卡点动态码牌；无任何按键、非安全设备。
 * NOTE: 当前阶段只做最小可运行验证（显示出厂状态 + 采样电量），
 *       对时/写种子/低功耗状态机等能力见 docs/architecture.md 的落地顺序。
 */
#include "battery.h"
#include "board_pins.h"
#include "ht1621.h"
#include "lcd_render.h"
#include "stm32l031xx.h"

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

    /* NOTE: keystore 尚未实现，设备当前必然处于"未写入种子"状态：没有种子就
     *       没有有效口令，数字位留空（空屏比错误码稳妥），DOT1 常亮提示配置。
     *       状态语义与"故障时清空数字位"见 docs/requirements.md §4.0。 */
    (void)lcd_render_show(0U, 0U, LCD_DOT_NO_SEED);

    /* TODO: 电量阈值上报 DOT4（FR-7 / §4.1）在状态机落地后接入 */
    battery_init();
    (void)battery_read_mv_filtered(1U);

    for (;;) {
        /* TODO: 主循环/低功耗状态机在本阶段之后实现，见 docs/architecture.md */
        __asm volatile ("wfi");
    }
}
