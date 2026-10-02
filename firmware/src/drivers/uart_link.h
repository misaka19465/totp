/**
 * @file    uart_link.h
 * @brief   耳机口串口链路层：帧编解码、CRC、重同步与会话状态机。
 *
 * 本层只认帧，不解释命令语义
 * （命令分派属 app/proto）；对 CRC 合法但 PAYLOAD > 64 的帧，
 * 自动回单字节 ERR_LEN，不向上传递。
 *
 * 链路状态机：
 *   SLEEP  —— USART/HSI16 关闭，PA10 下降沿 EXTI 等待唤醒；
 *   WAKE   —— 收到边沿后打开 USART，300 ms 内未收到 SOF 则回 SLEEP；
 *   ACTIVE —— 每次收到 CRC 合法帧刷新 10 s 会话定时，超时回 SLEEP。
 *
 * NOTE: 时间以调用方传入的毫秒计数为准（无内部时基），超时判断对 32 bit
 *       回绕安全；本模块只需在清醒期间被周期调用，典型由主循环/电源状态机驱动。
 *       流式解析只占 64 B 载荷缓冲 + 少量状态，低于协议预算的 96 B。
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 协议允许的最大 PAYLOAD */
#define UART_LINK_MAX_PAYLOAD        64U
/** 唤醒后等待 SOF 的窗口（T_wake） */
#define UART_LINK_WAKE_WINDOW_MS     300U
/** 自 SOF 起收完整帧的期限（T_frame_rx） */
#define UART_LINK_FRAME_TIMEOUT_MS   100U
/** 最后一个合法帧之后的会话保持时间（T_session） */
#define UART_LINK_SESSION_TIMEOUT_MS 10000U

/** 帧 SOF 与响应位 */
#define UART_LINK_SOF                0xAAU
#define UART_LINK_RESPONSE_BIT       0x80U

/** 链路状态 */
typedef enum {
    UART_LINK_SLEEP = 0,
    UART_LINK_WAKE,
    UART_LINK_ACTIVE,
} uart_link_state_t;

/** 一帧请求（CRC 已校验，LEN ≤ 64） */
typedef struct {
    uint8_t cmd;                        /**< 命令码（设备收到的请求码）  */
    uint8_t seq;                        /**< 上位机序号，设备原样回显      */
    uint8_t len;                        /**< payload 有效字节数（0..64）   */
    uint8_t payload[UART_LINK_MAX_PAYLOAD];
} uart_link_frame_t;

/**
 * @brief  初始化引脚与 EXTI，链路进入 SLEEP。
 * @note   USART/HSI16 保持关闭；不阻塞。
 */
void uart_link_init(void);

/**
 * @brief  强制唤醒链路（打开 USART，进入 WAKE 并开 300 ms 窗口）。
 * @param  now_ms 当前毫秒计数。
 * @note   正常唤醒路径由 uart_link_poll() 依据 plug_detect 事件自动完成；
 *         本函数供复位后/IWDG 唤醒等需要主动恢复收发的场景使用。
 */
void uart_link_wake(uint32_t now_ms);

/** 关闭 USART/HSI16 并回到 SLEEP；清除休眠期间无意义的旧边沿事件。 */
void uart_link_sleep(void);

/** 当前链路状态。 */
uart_link_state_t uart_link_state(void);

/**
 * @brief  推进链路状态机并取出至多一帧。
 * @param  now_ms    当前毫秒计数（单调递增，可回绕）。
 * @param  frame_out 输出帧；收到完整合法请求时填充并返回 true。
 * @return 取到帧返回 true；否则 false（未唤醒/超时/帧不完整/CRC 错均返回 false）。
 * @note   SLEEP 时检测 plug_detect 事件并自动唤醒；驱动 300 ms 唤醒窗口、
 *         100 ms 帧超时与 10 s 会话超时；LEN > 64 的 CRC 合法帧自动回
 *         ERR_LEN 并计入会话活动，不返回给调用方。
 */
bool uart_link_poll(uint32_t now_ms, uart_link_frame_t *frame_out);

/**
 * @brief  组帧并阻塞发送。
 * @param  cmd     命令码（响应须自带 |0x80）
 * @param  seq     序号
 * @param  payload 载荷（len == 0 时可为 NULL）
 * @param  len     载荷长度，须 ≤ 64
 * @return 链路处于唤醒状态且参数合法时发送并返回 true；否则不发送返回 false。
 */
bool uart_link_send(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint8_t len);

/**
 * @brief  组响应帧：CMD|0x80、SEQ 回显、PAYLOAD = [status] + data。
 * @return 发送成功返回 true；request 为 NULL、data_len > 63 或链路休眠返回 false。
 */
bool uart_link_send_response(const uart_link_frame_t *request, uint8_t status,
                             const uint8_t *data, uint8_t data_len);

#endif /* UART_LINK_H */
