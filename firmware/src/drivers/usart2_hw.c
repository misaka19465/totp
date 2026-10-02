/**
 * @file    usart2_hw.c
 * @brief   耳机口 USART2 寄存器级传输实现（PA9=TX / PA10=RX，HSI16，115200）。
 */
#include "usart2_hw.h"

#include <stddef.h>

#include "board_pins.h"
#include "stm32l031xx.h"

/* NOTE: HSI16 = 16 MHz，OVER8=0 时 BRR = fCK / 波特率 = 16e6 / 115200 ≈ 138.9，
 *       取最接近的 139：实际 115108 Bd，误差 -0.08%，远小于 8N1 容差。 */
#define USART2_BRR_115200   139U
#define USART2_AF_USART2    4U

/* NOTE: 线上连续字节间隔 = 10 bit / 115200 ≈ 87 µs，而主循环一圈（RTC 读 +
 *       喂狗 + 调度）明显长于一个字节时间；USART2 无 FIFO，纯轮询收必然
 *       ORE 丢字节（2026-10-02 实机：连续帧无响应、2 ms 间隔帧正常）。
 *       故 RXNE 中断把字节落环形缓冲，解析器按主循环节奏取用；
 *       96 B ≈ 8 ms 线上余量，可吸收一次 TOTP 计算（HMAC-SHA1）期间的到达。 */
#define USART2_RX_RING_SIZE 96U

static bool s_on = false;

static volatile uint8_t s_rx_ring[USART2_RX_RING_SIZE];
static volatile uint8_t s_rx_head;       /* 写入游标（仅中断上下文修改） */
static volatile uint8_t s_rx_tail;       /* 读取游标（仅主循环修改）     */
static volatile uint32_t s_rx_dropped;   /* 环形缓冲溢出计数（SWD 可读） */

/** USART2 中断：字节入环形缓冲并清错误标志（ORE/FE/NE/PE）。 */
void USART2_IRQHandler(void)
{
    const uint32_t isr = USART2->ISR;

    if ((isr & USART_ISR_RXNE) != 0U) {
        const uint8_t byte = (uint8_t)USART2->RDR;   /* 读 RDR 清 RXNE */
        const uint8_t next = (uint8_t)((s_rx_head + 1U) % USART2_RX_RING_SIZE);
        if (next != s_rx_tail) {
            s_rx_ring[s_rx_head] = byte;
            s_rx_head = next;
        } else {
            s_rx_dropped++;
        }
    }

    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U) {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    }
}

/** 休眠态引脚：PA9 输出高（避免上位机 RX 悬空拾噪），PA10 上拉输入（EXTI 唤醒源） */
static void pins_to_sleep(void)
{
    UART_TX_PORT->MODER =
        (UART_TX_PORT->MODER & ~(3U << (UART_TX_PIN * 2U))) | (1U << (UART_TX_PIN * 2U));
    UART_TX_PORT->OTYPER &= ~(1U << UART_TX_PIN);
    UART_TX_PORT->PUPDR &= ~(3U << (UART_TX_PIN * 2U));
    UART_TX_PORT->BSRR = (1U << UART_TX_PIN);

    UART_RX_PORT->MODER &= ~(3U << (UART_RX_PIN * 2U));
    UART_RX_PORT->PUPDR =
        (UART_RX_PORT->PUPDR & ~(3U << (UART_RX_PIN * 2U))) | (1U << (UART_RX_PIN * 2U));
}

/** 复用态引脚：PA9/PA10 = AF4（USART2），RX 保留上拉以免插头半插入时悬空 */
static void pins_to_af(void)
{
    UART_TX_PORT->MODER =
        (UART_TX_PORT->MODER & ~(3U << (UART_TX_PIN * 2U))) | (2U << (UART_TX_PIN * 2U));
    UART_TX_PORT->OTYPER &= ~(1U << UART_TX_PIN);
    UART_TX_PORT->AFR[1] =
        (UART_TX_PORT->AFR[1] & ~(0xFU << ((UART_TX_PIN - 8U) * 4U)))
        | (USART2_AF_USART2 << ((UART_TX_PIN - 8U) * 4U));

    UART_RX_PORT->MODER =
        (UART_RX_PORT->MODER & ~(3U << (UART_RX_PIN * 2U))) | (2U << (UART_RX_PIN * 2U));
    UART_RX_PORT->PUPDR =
        (UART_RX_PORT->PUPDR & ~(3U << (UART_RX_PIN * 2U))) | (1U << (UART_RX_PIN * 2U));
    UART_RX_PORT->AFR[1] =
        (UART_RX_PORT->AFR[1] & ~(0xFU << ((UART_RX_PIN - 8U) * 4U)))
        | (USART2_AF_USART2 << ((UART_RX_PIN - 8U) * 4U));
}

void usart2_hw_init_pins(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    pins_to_sleep();
}

void usart2_hw_power_on(void)
{
    if (s_on) {
        return;
    }

    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;

    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0U) {
    }

    RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_USART2SEL_Msk) | RCC_CCIPR_USART2SEL_1;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    pins_to_af();

    s_rx_head = 0U;
    s_rx_tail = 0U;

    USART2->CR1 = 0U;
    USART2->BRR = USART2_BRR_115200;
    USART2->CR2 = 0U;   /* 1 停止位，无 LIN/同步模式 */
    USART2->CR3 = 0U;   /* 无流控 */
    USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF |
                  USART_ICR_PECF | USART_ICR_IDLECF | USART_ICR_TCCF |
                  USART_ICR_CMCF | USART_ICR_WUCF;
    (void)USART2->RDR;  /* 读 RDR 清 RXNE */
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    NVIC_ClearPendingIRQ(USART2_IRQn);
    NVIC_EnableIRQ(USART2_IRQn);

    s_on = true;
}

void usart2_hw_power_off(void)
{
    if (s_on) {
        /* NOTE: 等最后一个字节完整上线再关时钟，避免截断响应帧。 */
        while ((USART2->ISR & USART_ISR_TC) == 0U) {
        }
        USART2->CR1 = 0U;
        s_on = false;
    }

    s_rx_head = 0U;
    s_rx_tail = 0U;

    RCC->APB1ENR &= ~RCC_APB1ENR_USART2EN;
    RCC->CR &= ~RCC_CR_HSION;
    pins_to_sleep();
}

bool usart2_hw_is_on(void)
{
    return s_on;
}

void usart2_hw_send_byte(uint8_t byte)
{
    while ((USART2->ISR & USART_ISR_TXE) == 0U) {
    }
    USART2->TDR = byte;
    while ((USART2->ISR & USART_ISR_TC) == 0U) {
    }
}

bool usart2_hw_try_receive(uint8_t *byte)
{
    if (byte == NULL || s_rx_tail == s_rx_head) {
        return false;
    }
    *byte = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint8_t)((s_rx_tail + 1U) % USART2_RX_RING_SIZE);
    return true;
}
