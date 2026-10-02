/**
 * @file    plug_detect.h
 * @brief   插头插入检测：PA10（UART_RX）下降沿 EXTI 唤醒。
 *
 * 硬件没有专用检测脚（docs/serial-protocol.md §2.1），插入只能靠 RX 线的活动推断：
 * 上位机唤醒前导码（00 00 00 00）的第一个下降沿触发 EXTI，设备据此开 USART。
 *
 * NOTE: 本模块只管"发现边沿"；唤醒窗口（300 ms）与会话超时（10 s）由
 *       uart_link 状态机维护，见 docs/serial-protocol.md §3。
 *
 * FIXME: 触发条件——当前硬件批次没有独立插入检测引脚，只能把 RX 数据边沿
 *        当作插入事件：静电、插拔抖动都会误唤醒，且上位机每次建会话都必须
 *        发前导码、设备必须开 300 ms 窗口兜底。修正——下一版硬件应为耳机座
 *        增加独立插入检测引脚（任一 EXTI 可用 GPIO，插头插入时电平翻转），
 *        唤醒源改为该引脚。回归检查——改版后验证插入/拔出/抖动下不漏唤醒、
 *        不误唤醒，然后删除 RX 边沿唤醒路径，简化 uart_link 唤醒窗口与
 *        serial-protocol.md §3 的前导码流程，并同步 hardware.md H7。
 */
#ifndef PLUG_DETECT_H
#define PLUG_DETECT_H

#include <stdbool.h>

/**
 * @brief  配置 PA10 为上拉输入 + EXTI 下降沿中断，并使能 NVIC。
 * @note   EXTI 中断已使能，可直接作为 Stop 模式唤醒源；
 *         超时/清标志逻辑见 uart_link_sleep()。
 */
void plug_detect_init(void);

/**
 * @brief  读取并清除"检测到 RX 下降沿"事件。
 * @return 自上次调用以来出现过下降沿返回 true。
 */
bool plug_detect_wake_pending(void);

#endif /* PLUG_DETECT_H */
