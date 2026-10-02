/**
 * @file    uart_link.c
 * @brief   耳机口串口链路层实现（帧解析/CRC/重同步/会话状态机）。
 */
#include "uart_link.h"

#include "crc16.h"
#include "plug_detect.h"
#include "usart2_hw.h"

typedef enum {
    RX_WAIT_SOF = 0,
    RX_CMD,
    RX_SEQ,
    RX_LEN,
    RX_PAYLOAD,
    RX_CRC_LO,
    RX_CRC_HI,
} rx_state_t;

typedef struct {
    rx_state_t state;
    uint32_t frame_start_ms;                /* SOF 到达时刻，用于 T_frame_rx */
    uint16_t crc;                           /* CMD..PAYLOAD 的运行 CRC      */
    uint16_t crc_rx;                        /* 线上收到的 CRC（小端）        */
    uint8_t cmd;
    uint8_t seq;
    uint8_t len;
    uint8_t remaining;                      /* PAYLOAD 剩余字节数           */
    uint8_t filled;                         /* 已存入 payload 的字节数      */
    uint8_t payload[UART_LINK_MAX_PAYLOAD];
} rx_ctx_t;

static rx_ctx_t s_rx;
static uart_link_state_t s_state = UART_LINK_SLEEP;
static uint32_t s_wake_start_ms = 0U;
static uint32_t s_last_frame_ms = 0U;

static void rx_reset(void)
{
    s_rx.state = RX_WAIT_SOF;
    s_rx.frame_start_ms = 0U;
    s_rx.crc = CRC16_CCITT_FALSE_INIT;
    s_rx.crc_rx = 0U;
    s_rx.cmd = 0U;
    s_rx.seq = 0U;
    s_rx.len = 0U;
    s_rx.remaining = 0U;
    s_rx.filled = 0U;
}

void uart_link_init(void)
{
    usart2_hw_init_pins();
    plug_detect_init();
    rx_reset();
    usart2_hw_power_off();
    s_state = UART_LINK_SLEEP;
    s_wake_start_ms = 0U;
    s_last_frame_ms = 0U;
}

void uart_link_wake(uint32_t now_ms)
{
    (void)plug_detect_wake_pending();   /* 清掉触发本次唤醒的边沿 */
    usart2_hw_power_on();
    rx_reset();
    s_state = UART_LINK_WAKE;
    s_wake_start_ms = now_ms;
}

void uart_link_sleep(void)
{
    usart2_hw_power_off();
    rx_reset();
    /* ACTIVE 期间每个字节的起始位都会触发 EXTI，休眠前丢弃这些旧事件 */
    (void)plug_detect_wake_pending();
    s_state = UART_LINK_SLEEP;
}

uart_link_state_t uart_link_state(void)
{
    return s_state;
}

/**
 * @brief  逐字节推进接收解析器。
 * @return 收完一帧且 CRC 正确返回 true；否则 false。
 * @note   CRC 错帧静默丢弃，从下一个 0xAA 重新开始（规范 §4.4）；
 *         LEN 可达 255，超长帧只计数不存储，CRC 仍覆盖全部载荷。
 */
static bool rx_feed(uint8_t byte, uint32_t now_ms)
{
    switch (s_rx.state) {
    case RX_WAIT_SOF:
        if (byte == UART_LINK_SOF) {
            s_rx.crc = CRC16_CCITT_FALSE_INIT;
            s_rx.frame_start_ms = now_ms;
            s_rx.state = RX_CMD;
        }
        break;
    case RX_CMD:
        s_rx.cmd = byte;
        s_rx.crc = crc16_update(s_rx.crc, byte);
        s_rx.state = RX_SEQ;
        break;
    case RX_SEQ:
        s_rx.seq = byte;
        s_rx.crc = crc16_update(s_rx.crc, byte);
        s_rx.state = RX_LEN;
        break;
    case RX_LEN:
        s_rx.len = byte;
        s_rx.crc = crc16_update(s_rx.crc, byte);
        s_rx.remaining = byte;
        s_rx.filled = 0U;
        s_rx.state = (byte == 0U) ? RX_CRC_LO : RX_PAYLOAD;
        break;
    case RX_PAYLOAD:
        s_rx.crc = crc16_update(s_rx.crc, byte);
        if (s_rx.filled < UART_LINK_MAX_PAYLOAD) {
            s_rx.payload[s_rx.filled] = byte;
            s_rx.filled++;
        }
        s_rx.remaining--;
        if (s_rx.remaining == 0U) {
            s_rx.state = RX_CRC_LO;
        }
        break;
    case RX_CRC_LO:
        s_rx.crc_rx = byte;
        s_rx.state = RX_CRC_HI;
        break;
    case RX_CRC_HI:
        s_rx.crc_rx |= (uint16_t)byte << 8;
        s_rx.state = RX_WAIT_SOF;
        return (s_rx.crc_rx == s_rx.crc);
    }
    return false;
}

bool uart_link_poll(uint32_t now_ms, uart_link_frame_t *frame_out)
{
    if (s_state == UART_LINK_SLEEP) {
        if (!plug_detect_wake_pending()) {
            return false;
        }
        uart_link_wake(now_ms);
    }

    /* 先丢弃已超时的残帧，再收新字节：否则重传帧会被残帧解析器吞掉（§4.4） */
    if (s_rx.state != RX_WAIT_SOF &&
        (uint32_t)(now_ms - s_rx.frame_start_ms) >= UART_LINK_FRAME_TIMEOUT_MS) {
        rx_reset();
    }

    uint8_t byte = 0U;
    while (usart2_hw_try_receive(&byte)) {
        if (!rx_feed(byte, now_ms)) {
            continue;
        }

        /* CRC 合法帧：先刷新会话，再决定交付还是回 ERR_LEN */
        s_state = UART_LINK_ACTIVE;
        s_last_frame_ms = now_ms;

        if (s_rx.len > UART_LINK_MAX_PAYLOAD) {
            static const uint8_t err_len_payload[1] = { 0x01U };   /* ERR_LEN */
            (void)uart_link_send((uint8_t)(s_rx.cmd | UART_LINK_RESPONSE_BIT),
                                 s_rx.seq, err_len_payload, 1U);
            rx_reset();
            continue;
        }

        if (frame_out != NULL) {
            frame_out->cmd = s_rx.cmd;
            frame_out->seq = s_rx.seq;
            frame_out->len = s_rx.len;
            for (uint8_t i = 0U; i < s_rx.len; i++) {
                frame_out->payload[i] = s_rx.payload[i];
            }
            rx_reset();
            return true;
        }
        rx_reset();
    }

    if (s_rx.state != RX_WAIT_SOF) {
        /* 帧未收完时不判定窗口/会话超时，交给下一轮的帧内超时收口 */
        return false;
    }

    if (s_state == UART_LINK_WAKE &&
        (uint32_t)(now_ms - s_wake_start_ms) >= UART_LINK_WAKE_WINDOW_MS) {
        uart_link_sleep();
    } else if (s_state == UART_LINK_ACTIVE &&
               (uint32_t)(now_ms - s_last_frame_ms) >= UART_LINK_SESSION_TIMEOUT_MS) {
        uart_link_sleep();
    }
    return false;
}

bool uart_link_send(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint8_t len)
{
    if (s_state == UART_LINK_SLEEP || !usart2_hw_is_on() ||
        len > UART_LINK_MAX_PAYLOAD || (payload == NULL && len > 0U)) {
        return false;
    }

    uint16_t crc = CRC16_CCITT_FALSE_INIT;
    crc = crc16_update(crc, cmd);
    crc = crc16_update(crc, seq);
    crc = crc16_update(crc, len);
    for (uint8_t i = 0U; i < len; i++) {
        crc = crc16_update(crc, payload[i]);
    }

    usart2_hw_send_byte(UART_LINK_SOF);
    usart2_hw_send_byte(cmd);
    usart2_hw_send_byte(seq);
    usart2_hw_send_byte(len);
    for (uint8_t i = 0U; i < len; i++) {
        usart2_hw_send_byte(payload[i]);
    }
    usart2_hw_send_byte((uint8_t)(crc & 0xFFU));
    usart2_hw_send_byte((uint8_t)(crc >> 8));
    return true;
}

bool uart_link_send_response(const uart_link_frame_t *request, uint8_t status,
                             const uint8_t *data, uint8_t data_len)
{
    if (request == NULL || data_len > (UART_LINK_MAX_PAYLOAD - 1U) ||
        (data == NULL && data_len > 0U)) {
        return false;
    }

    uint8_t payload[UART_LINK_MAX_PAYLOAD];
    payload[0] = status;
    for (uint8_t i = 0U; i < data_len; i++) {
        payload[i + 1U] = data[i];
    }

    return uart_link_send((uint8_t)(request->cmd | UART_LINK_RESPONSE_BIT),
                          request->seq, payload, (uint8_t)(data_len + 1U));
}
