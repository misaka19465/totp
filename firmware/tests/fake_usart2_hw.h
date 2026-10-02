/**
 * @file    fake_usart2_hw.h
 * @brief   主机侧测试替身：捕获 uart_link 发出的字节并注入待收字节。
 */
#ifndef FAKE_USART2_HW_H
#define FAKE_USART2_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 清空收发缓冲并把电源状态置为关闭（模拟 uart_link_init 前后的上电初态） */
void fake_usart2_hw_reset(void);

/** 只清空发送捕获缓冲，保留电源与接收缓冲 */
void fake_usart2_hw_clear_tx(void);

/** 注入待接收字节（按顺序由 usart2_hw_try_receive 取出） */
void fake_usart2_hw_inject(const uint8_t *data, size_t len);

/** 捕获到的发送字节 */
const uint8_t *fake_usart2_hw_sent(void);
size_t fake_usart2_hw_sent_len(void);

/** 当前电源状态（由 power_on/power_off 驱动） */
bool fake_usart2_hw_is_on(void);

#endif /* FAKE_USART2_HW_H */
