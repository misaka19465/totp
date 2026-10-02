/**
 * @file    fake_usart2_hw.c
 * @brief   usart2_hw 的主机侧测试替身（见 fake_usart2_hw.h）。
 */
#include "fake_usart2_hw.h"

#include "usart2_hw.h"

#include <string.h>

#define FAKE_USART2_TX_MAX 512U
#define FAKE_USART2_RX_MAX 512U

static uint8_t s_tx[FAKE_USART2_TX_MAX];
static size_t s_tx_len = 0U;
static uint8_t s_rx[FAKE_USART2_RX_MAX];
static size_t s_rx_head = 0U;
static size_t s_rx_tail = 0U;
static bool s_on = false;

void fake_usart2_hw_reset(void)
{
    s_tx_len = 0U;
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_on = false;
    memset(s_tx, 0, sizeof s_tx);
    memset(s_rx, 0, sizeof s_rx);
}

void fake_usart2_hw_clear_tx(void)
{
    s_tx_len = 0U;
}

void fake_usart2_hw_inject(const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if (s_rx_tail < FAKE_USART2_RX_MAX) {
            s_rx[s_rx_tail] = data[i];
            s_rx_tail++;
        }
    }
}

const uint8_t *fake_usart2_hw_sent(void)
{
    return s_tx;
}

size_t fake_usart2_hw_sent_len(void)
{
    return s_tx_len;
}

bool fake_usart2_hw_is_on(void)
{
    return s_on;
}

/* ---- 与 usart2_hw.h 原型一致的测试实现 ---- */

void usart2_hw_init_pins(void)
{
}

void usart2_hw_power_on(void)
{
    s_on = true;
}

void usart2_hw_power_off(void)
{
    s_on = false;
}

bool usart2_hw_is_on(void)
{
    return s_on;
}

void usart2_hw_send_byte(uint8_t byte)
{
    if (s_tx_len < FAKE_USART2_TX_MAX) {
        s_tx[s_tx_len] = byte;
        s_tx_len++;
    }
}

bool usart2_hw_try_receive(uint8_t *byte)
{
    if (!s_on || byte == NULL || s_rx_head >= s_rx_tail) {
        return false;
    }
    *byte = s_rx[s_rx_head];
    s_rx_head++;
    return true;
}
