/**
 * @file    usart2_hw.h
 * @brief   耳机口 USART2 硬件传输层（PA9=TX / PA10=RX，115200 8N1）。
 *
 * 只做寄存器级引脚、时钟与字节收发，不含任何帧/超时逻辑（见 uart_link.h）。
 * 该接口与实现分离，主机侧测试用 fake_usart2_hw.c 替换（同 fake_ht1621 模式）。
 *
 * NOTE: 115200 需要 HSI16（16 MHz）作为 USART2 时钟；休眠时 USART2 与 HSI16
 *       全部关闭，PA9 输出高、PA10 上拉输入。
 */
#ifndef USART2_HW_H
#define USART2_HW_H

#include <stdbool.h>
#include <stdint.h>

/** 配置 PA9/PA10 为休眠态（PA9 输出高、PA10 上拉输入）。 */
void usart2_hw_init_pins(void);

/** 打开 HSI16 + USART2 并切 PA9/PA10 到 AF4；已打开时无副作用。 */
void usart2_hw_power_on(void);

/** 关闭 USART2 与 HSI16，并恢复休眠态引脚；未打开时只做引脚复位。 */
void usart2_hw_power_off(void);

/** USART2 是否打开（HSI16 + 收发使能）。 */
bool usart2_hw_is_on(void);

/**
 * @brief  阻塞发送一个字节。
 * @note   返回时该字节已完整上线（等待 TC），可安全随后关闭 USART。
 */
void usart2_hw_send_byte(uint8_t byte);

/**
 * @brief  非阻塞读取一个字节（来自 RXNE 中断的环形缓冲）。
 * @param  byte 输出；仅当返回 true 时有效
 * @return 收到字节返回 true，无数据返回 false
 * @note   字节在中断里入队，本函数只消费缓冲；调用间隙不再有 ORE 丢字节，
 *         但缓冲仅 96 B（≈8 ms 线上时间），解析方仍需按主循环节奏及时抽取。
 */
bool usart2_hw_try_receive(uint8_t *byte);

#endif /* USART2_HW_H */
