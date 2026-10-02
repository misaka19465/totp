/**
 * @file    plug_detect.c
 * @brief   PA10 下降沿 EXTI 唤醒实现（无检测脚，见 plug_detect.h）。
 *
 * FIXME: 见 plug_detect.h 的 FIXME——下一版硬件改用独立插入检测引脚取代
 *        本 RX 边沿推断方案；改版时删除此处的 EXTI10 配置。
 */
#include "plug_detect.h"

#include "board_pins.h"
#include "stm32l031xx.h"

/* NOTE: PA10 落在 EXTI4_15 向量（EXTI 线 10），EXTICR3 的 EXTI10 字段选择端口。 */
#define PLUG_DETECT_LINE       (1UL << UART_RX_PIN)

static volatile bool s_wake_pending = false;

void plug_detect_init(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    /* 上拉输入：无插头时线被拉高，插入上位机后的第一个低电平即为前导码起始位 */
    UART_RX_PORT->MODER &= ~(3U << (UART_RX_PIN * 2U));
    UART_RX_PORT->PUPDR =
        (UART_RX_PORT->PUPDR & ~(3U << (UART_RX_PIN * 2U))) | (1U << (UART_RX_PIN * 2U));

    const uint32_t port_field_shift = ((UART_RX_PIN - 8U) * 4U);
    SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~(0xFUL << port_field_shift))
                        | (0x0UL << port_field_shift);   /* 0x0 = PA */

    EXTI->IMR |= PLUG_DETECT_LINE;
    EXTI->FTSR |= PLUG_DETECT_LINE;
    EXTI->RTSR &= ~PLUG_DETECT_LINE;
    EXTI->PR = PLUG_DETECT_LINE;   /* 写 1 清残留挂起位 */

    s_wake_pending = false;
    NVIC_EnableIRQ(EXTI4_15_IRQn);
}

bool plug_detect_wake_pending(void)
{
    bool pending = s_wake_pending;
    if ((EXTI->PR & PLUG_DETECT_LINE) != 0U) {
        EXTI->PR = PLUG_DETECT_LINE;
        pending = true;
    }
    s_wake_pending = false;
    return pending;
}

/** EXTI4_15 中断服务：PA10 下降沿置位唤醒标志（Stop 模式唤醒路径） */
void EXTI4_15_IRQHandler(void)
{
    if ((EXTI->PR & PLUG_DETECT_LINE) != 0U) {
        EXTI->PR = PLUG_DETECT_LINE;
        s_wake_pending = true;
    }
}
