/**
 * @file    test_uart_link.c
 * @brief   uart_link 单元测试：CRC、组帧、解析/重同步、超时与会话状态机。
 *
 * 帧字节取自固定测试向量（由 tools/provision 的 Python 参考实现生成）；
 * 构造的超长/坏 CRC 帧用逐位 CRC 独立生成，
 * 与实现的半字节表算法交叉校验。
 */
#include "test_util.h"

#include "crc16.h"
#include "fake_plug_detect.h"
#include "fake_usart2_hw.h"
#include "uart_link.h"

/* ---- 测试向量 ---- */
static const uint8_t PING_REQ[]     = { 0xaa, 0x01, 0x00, 0x00, 0xac, 0xfb };
static const uint8_t SET_TIME_REQ[] = {
    0xaa, 0x03, 0x2a, 0x08, 0x00, 0x20, 0xe9, 0xf9, 0xa0, 0x01, 0x00, 0x00, 0x86, 0xcf,
};
static const uint8_t WRITE_SLOT_REQ[] = {
    0xaa, 0x05, 0x01, 0x15, 0x01, 0x06, 0x1e, 0x00, 0x10, 0x4a, 0x42, 0x53, 0x57, 0x59,
    0x33, 0x44, 0x50, 0x45, 0x48, 0x50, 0x4b, 0x33, 0x50, 0x58, 0x50, 0xb0, 0x97,
};
static const uint8_t PING_RESP_DATA[10] = {
    0x01, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t BAD_PING[] = { 0xaa, 0x01, 0x00, 0x00, 0xac, 0x04 };

/* LEN=65 的 WRITE_SLOT 请求（CRC 正确、超协议上限，见 Python 参考实现生成） */
static const uint8_t LEN65_REQ[] = {
    0xaa, 0x05, 0x2a, 0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0xb6,
    0xea,
};

/** 独立的逐位 CRC-16/CCITT-FALSE，用于交叉校验与构造测试帧 */
static uint16_t ref_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                          : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/** 按线格式组一帧（含 CRC，小端），返回总字节数 */
static size_t build_frame(uint8_t *out, uint8_t cmd, uint8_t seq,
                          const uint8_t *payload, uint8_t len)
{
    out[0] = UART_LINK_SOF;
    out[1] = cmd;
    out[2] = seq;
    out[3] = len;
    for (uint8_t i = 0U; i < len; i++) {
        out[4U + i] = payload[i];
    }
    const uint16_t crc = ref_crc16(&out[1], (size_t)len + 3U);
    out[4U + len] = (uint8_t)(crc & 0xFFU);
    out[5U + len] = (uint8_t)(crc >> 8);
    return (size_t)len + 6U;
}

/** 逐字节喂给链路，返回是否取到帧（now 不变，模拟一次调用内的连续字节） */
static bool feed_all(const uint8_t *bytes, size_t len, uint32_t now_ms,
                     uart_link_frame_t *frame_out)
{
    for (size_t i = 0U; i < len; i++) {
        fake_usart2_hw_inject(&bytes[i], 1U);
        if (uart_link_poll(now_ms, frame_out)) {
            return true;
        }
    }
    return false;
}

void test_uart_link(void)
{
    uart_link_frame_t frame;

    /* ---- CRC 自测向量与交叉校验 ---- */
    CHECK_U32(crc16_ccitt_false((const uint8_t *)"123456789", 9U), 0x29B1U);
    CHECK_U32(crc16_ccitt_false(NULL, 0U), 0xFFFFU);
    static const uint8_t crc_sample[] = { 0x00, 0xaa, 0x55, 0xff, 0x10, 0x21 };
    CHECK_U32(crc16_ccitt_false(crc_sample, sizeof crc_sample),
              ref_crc16(crc_sample, sizeof crc_sample));
    CHECK_U32(crc16_ccitt_false(&SET_TIME_REQ[1], 11U), ref_crc16(&SET_TIME_REQ[1], 11U));

    /* ---- 初始化后处于 SLEEP，USART 关闭 ---- */
    fake_usart2_hw_reset();
    fake_plug_detect_reset();
    uart_link_init();
    CHECK(uart_link_state() == UART_LINK_SLEEP);
    CHECK(!fake_usart2_hw_is_on());

    /* ---- 插头边沿唤醒：进入 WAKE 并打开 USART ---- */
    fake_plug_detect_set_pending();
    CHECK(!uart_link_poll(1000U, &frame));
    CHECK(uart_link_state() == UART_LINK_WAKE);
    CHECK(fake_usart2_hw_is_on());

    /* ---- 300 ms 窗口内无 SOF 回 SLEEP ---- */
    CHECK(!uart_link_poll(1299U, &frame));
    CHECK(uart_link_state() == UART_LINK_WAKE);
    CHECK(!uart_link_poll(1300U, &frame));
    CHECK(uart_link_state() == UART_LINK_SLEEP);
    CHECK(!fake_usart2_hw_is_on());

    /* ---- PING 请求向量解码，会话进入 ACTIVE ---- */
    uart_link_wake(2000U);
    CHECK(feed_all(PING_REQ, sizeof PING_REQ, 2100U, &frame));
    CHECK_U32(frame.cmd, 0x01U);
    CHECK_U32(frame.seq, 0x00U);
    CHECK_U32(frame.len, 0x00U);
    CHECK(uart_link_state() == UART_LINK_ACTIVE);

    /* ---- PING 响应组帧与向量逐字节一致 ---- */
    fake_usart2_hw_clear_tx();
    CHECK(uart_link_send_response(&frame, 0x00U, PING_RESP_DATA, 10U));
    CHECK(fake_usart2_hw_sent_len() == 17U);
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa81000b0001000140000000000000386e");

    /* ---- SET_TIME 请求向量解码 ---- */
    CHECK(feed_all(SET_TIME_REQ, sizeof SET_TIME_REQ, 2200U, &frame));
    CHECK_U32(frame.cmd, 0x03U);
    CHECK_U32(frame.seq, 0x2aU);
    CHECK_U32(frame.len, 8U);
    CHECK_HEX(frame.payload, frame.len, "0020e9f9a0010000");

    /* ---- WRITE_SLOT 21 B 载荷向量解码 ---- */
    CHECK(feed_all(WRITE_SLOT_REQ, sizeof WRITE_SLOT_REQ, 2300U, &frame));
    CHECK_U32(frame.cmd, 0x05U);
    CHECK_U32(frame.seq, 0x01U);
    CHECK_U32(frame.len, 0x15U);
    CHECK_U32(frame.payload[0], 0x01U);   /* algo = SHA1 */
    CHECK_U32(frame.payload[1], 0x06U);   /* digits = 6  */
    CHECK_U32(frame.payload[4], 0x10U);   /* secret_len = 16 字符 */
    CHECK_HEX(&frame.payload[5], 16U, "4a425357593344504548504b33505850");

    /* ---- 垃圾字节与坏 CRC 帧被丢弃，随后合法帧可重同步 ---- */
    static const uint8_t garbage[] = { 0x00, 0x7e, 0x55 };
    fake_usart2_hw_inject(garbage, sizeof garbage);
    CHECK(!uart_link_poll(2400U, &frame));
    CHECK(!feed_all(BAD_PING, sizeof BAD_PING, 2400U, &frame));
    CHECK(feed_all(PING_REQ, sizeof PING_REQ, 2400U, &frame));
    CHECK_U32(frame.cmd, 0x01U);

    /* ---- 一次 poll 只交付一帧，连续两帧可先后取出 ---- */
    uint8_t two_frames[sizeof PING_REQ * 2U];
    for (size_t i = 0U; i < sizeof PING_REQ; i++) {
        two_frames[i] = PING_REQ[i];
        two_frames[i + sizeof PING_REQ] = PING_REQ[i];
    }
    fake_usart2_hw_inject(two_frames, sizeof two_frames);
    CHECK(uart_link_poll(2500U, &frame));
    CHECK(uart_link_poll(2500U, &frame));
    CHECK(!uart_link_poll(2500U, &frame));

    /* ---- 会话超时：最后一个合法帧后 10 s 回 SLEEP ---- */
    uart_link_init();
    uart_link_wake(10000U);
    CHECK(feed_all(PING_REQ, sizeof PING_REQ, 10000U, &frame));
    CHECK(!uart_link_poll(19999U, &frame));
    CHECK(uart_link_state() == UART_LINK_ACTIVE);
    CHECK(!uart_link_poll(20000U, &frame));
    CHECK(uart_link_state() == UART_LINK_SLEEP);
    CHECK(!fake_usart2_hw_is_on());

    /* ---- 每个合法帧都刷新会话定时 ---- */
    uart_link_wake(30000U);
    CHECK(feed_all(PING_REQ, sizeof PING_REQ, 30000U, &frame));
    CHECK(feed_all(PING_REQ, sizeof PING_REQ, 39000U, &frame));
    CHECK(!uart_link_poll(48999U, &frame));
    CHECK(uart_link_state() == UART_LINK_ACTIVE);
    CHECK(!uart_link_poll(49000U, &frame));
    CHECK(uart_link_state() == UART_LINK_SLEEP);

    /* ---- 帧内 100 ms 超时丢弃残帧，重传的完整帧仍可解析 ---- */
    uart_link_wake(50000U);
    fake_usart2_hw_inject(SET_TIME_REQ, 3U);   /* SOF + CMD + SEQ，随后的字节丢失 */
    CHECK(!uart_link_poll(50000U, &frame));
    CHECK(!uart_link_poll(50099U, &frame));
    CHECK(!uart_link_poll(50100U, &frame));    /* 到期，残帧被丢弃 */
    CHECK(feed_all(SET_TIME_REQ, sizeof SET_TIME_REQ, 50100U, &frame));
    CHECK_U32(frame.cmd, 0x03U);
    CHECK(uart_link_state() == UART_LINK_ACTIVE);

    /* ---- 残帧到期与新帧同时到达：先丢残帧再解析，重传不需要额外重试 ---- */
    uart_link_wake(55000U);
    fake_usart2_hw_inject(SET_TIME_REQ, 3U);
    CHECK(!uart_link_poll(55000U, &frame));
    fake_usart2_hw_inject(SET_TIME_REQ, sizeof SET_TIME_REQ);   /* 重传整帧 */
    CHECK(uart_link_poll(55100U, &frame));
    CHECK_U32(frame.cmd, 0x03U);

    /* ---- LEN > 64：CRC 正确时自动回 ERR_LEN，不计入交付 ---- */
    uart_link_init();
    uart_link_wake(60000U);
    fake_usart2_hw_clear_tx();
    CHECK(!feed_all(LEN65_REQ, sizeof LEN65_REQ, 60000U, &frame));
    CHECK(fake_usart2_hw_sent_len() == 7U);
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(), "aa852a0101aa87");
    CHECK(uart_link_state() == UART_LINK_ACTIVE);
    CHECK(!uart_link_poll(69999U, &frame));
    CHECK(uart_link_state() == UART_LINK_ACTIVE);   /* 超长帧同样刷新会话 */
    CHECK(!uart_link_poll(70000U, &frame));
    CHECK(uart_link_state() == UART_LINK_SLEEP);

    /* ---- 64 B 载荷边界帧可完整解析 ---- */
    uart_link_wake(80000U);
    uint8_t big_payload[64];
    for (uint8_t i = 0U; i < 64U; i++) {
        big_payload[i] = (uint8_t)(i * 7U + 3U);
    }
    uint8_t big_frame[70];
    const size_t big_len = build_frame(big_frame, 0x05U, 0x11U, big_payload, 64U);
    CHECK(big_len == sizeof big_frame);
    CHECK(feed_all(big_frame, big_len, 80000U, &frame));
    CHECK_U32(frame.len, 64U);
    CHECK(memcmp(frame.payload, big_payload, sizeof big_payload) == 0);

    /* ---- 响应载荷边界：63 B 数据可发，64 B 数据拒绝 ---- */
    fake_usart2_hw_clear_tx();
    uint8_t resp_data[64];
    for (uint8_t i = 0U; i < 64U; i++) {
        resp_data[i] = (uint8_t)(0x80U + i);
    }
    CHECK(uart_link_send_response(&frame, 0x00U, resp_data, 63U));
    CHECK(fake_usart2_hw_sent_len() == 70U);
    CHECK_U32(fake_usart2_hw_sent()[1], 0x85U);
    CHECK_U32(fake_usart2_hw_sent()[3], 64U);      /* LEN = status + 63 */
    CHECK_U32(fake_usart2_hw_sent()[4], 0x00U);
    CHECK_U32(fake_usart2_hw_sent()[5], 0x80U);
    fake_usart2_hw_clear_tx();
    CHECK(!uart_link_send_response(&frame, 0x00U, resp_data, 64U));
    CHECK(fake_usart2_hw_sent_len() == 0U);

    /* ---- SLEEP 时拒绝发送 ---- */
    uart_link_sleep();
    fake_usart2_hw_clear_tx();
    CHECK(!uart_link_send(0x81U, 0x00U, NULL, 0U));
    CHECK(fake_usart2_hw_sent_len() == 0U);

    /* ---- frame_out = NULL 时仍需驱动状态机并消费合法帧 ---- */
    fake_plug_detect_set_pending();
    CHECK(!uart_link_poll(90000U, NULL));               /* 唤醒，无帧 */
    fake_usart2_hw_inject(PING_REQ, sizeof PING_REQ);
    CHECK(!uart_link_poll(90000U, NULL));               /* 帧被消费但不交付 */
    CHECK(uart_link_state() == UART_LINK_ACTIVE);
}
