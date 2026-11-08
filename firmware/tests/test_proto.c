/**
 * @file    test_proto.c
 * @brief   proto 单元测试：全部命令的响应字节、
 *          字段校验与错误路径。
 *
 * 链路层用真实 uart_link + fake_usart2_hw/plug_detect，响应帧经 fake 捕获后
 * 与固定测试向量（或用逐位 CRC 独立组帧的期望值）逐字节比对；keystore 用真实
 * 实现 + fake_nvm（覆盖 EEPROM 失败路径），clock_rtc 用 fake_clock_rtc
 * 提供可控时间、对时状态与校准读数。
 */
#include "test_util.h"

#include <string.h>

#include "fake_clock_rtc.h"
#include "fake_nvm.h"
#include "fake_plug_detect.h"
#include "fake_usart2_hw.h"
#include "clock_rtc.h"
#include "keystore.h"
#include "proto.h"
#include "uart_link.h"

/* ---- 测试向量（请求帧） ---- */
static const uint8_t PING_REQ[] = { 0xaa, 0x01, 0x00, 0x00, 0xac, 0xfb };
static const uint8_t SET_TIME_REQ[] = {
    0xaa, 0x03, 0x2a, 0x08, 0x00, 0x20, 0xe9, 0xf9, 0xa0, 0x01, 0x00, 0x00, 0x86, 0xcf,
};
/* 2026-10-02T00:00:00Z = 1790899200000 ms（对时向量） */
#define SET_TIME_MS 1790899200000ULL

/* WRITE_SLOT 载荷（向量去掉帧头与 CRC）：algo=1、period=30、
   secret=JBSWY3DPEHPK3PXP（16 字符） */
static const uint8_t WRITE_PAYLOAD_D6[21] = {
    0x01, 0x06, 0x1e, 0x00, 0x10, 0x4a, 0x42, 0x53, 0x57, 0x59, 0x33,
    0x44, 0x50, 0x45, 0x48, 0x50, 0x4b, 0x33, 0x50, 0x58, 0x50,
};
static const uint8_t WRITE_PAYLOAD_D4[21] = {
    0x01, 0x04, 0x1e, 0x00, 0x10, 0x4a, 0x42, 0x53, 0x57, 0x59, 0x33,
    0x44, 0x50, 0x45, 0x48, 0x50, 0x4b, 0x33, 0x50, 0x58, 0x50,
};

static proto_status_t s_status;
static uint32_t s_now_ms = 1000U;

/* ---- 测试辅助 ---- */

/** 独立的逐位 CRC-16/CCITT-FALSE，与固件半字节表实现交叉校验 */
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

/** 逐字节喂给链路并取帧（now 不变，模拟一次调用内的连续字节） */
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

/** 复位全部替身、链路与默认状态快照 */
static void setup(void)
{
    fake_nvm_reset();
    fake_usart2_hw_reset();
    fake_plug_detect_reset();
    fake_clock_rtc_reset();
    uart_link_init();

    memset(&s_status, 0, sizeof s_status);
    s_status.battery_level = BATTERY_LEVEL_NORMAL;
    s_status.reset_cause = PROTO_RESET_POR;
    s_status.uptime_s = 0U;
    fake_clock_rtc_set_last_set_age_s(UINT32_MAX);

    s_now_ms = 1000U;
}

/** 唤醒链路、喂入一帧、清 TX、交给 proto 处理；返回发送是否成功 */
static bool send_raw(const uint8_t *bytes, size_t len)
{
    uart_link_frame_t frame;
    s_now_ms += 1000U;
    uart_link_wake(s_now_ms);
    if (!feed_all(bytes, len, s_now_ms, &frame)) {
        return false;
    }
    fake_usart2_hw_clear_tx();
    return proto_handle_request(&frame, &s_status, NULL);
}

static bool send_request(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint8_t len)
{
    uint8_t bytes[UART_LINK_MAX_PAYLOAD + 6U];
    const size_t bytes_len = build_frame(bytes, cmd, seq, payload, len);
    return send_raw(bytes, bytes_len);
}

/** 核对 fake 捕获的发送内容 = 一条响应帧（CMD|0x80、SEQ 回显、[status]+data） */
static void expect_response(uint8_t cmd, uint8_t seq, uint8_t status,
                            const uint8_t *data, uint8_t data_len)
{
    uint8_t payload[UART_LINK_MAX_PAYLOAD];
    uint8_t expected[UART_LINK_MAX_PAYLOAD + 6U];
    payload[0] = status;
    if (data_len > 0U) {
        memcpy(&payload[1], data, data_len);
    }
    const uint8_t payload_len = (uint8_t)(data_len + 1U);
    const size_t expected_len = build_frame(expected, (uint8_t)(cmd | 0x80U), seq,
                                            payload, payload_len);

    g_checks++;
    if (fake_usart2_hw_sent_len() != expected_len ||
        memcmp(fake_usart2_hw_sent(), expected, expected_len) != 0) {
        g_failures++;
        printf("FAIL %s:%d: 响应与期望不符（cmd=0x%02X seq=%u status=0x%02X）\n",
               __FILE__, __LINE__, cmd, seq, status);
        printf("  实际 = ");
        test_print_hex(fake_usart2_hw_sent(), fake_usart2_hw_sent_len());
        printf("\n  期望 = ");
        test_print_hex(expected, expected_len);
        printf("\n");
    }
}

static void put_u64le(uint8_t *p, uint64_t value)
{
    for (uint8_t i = 0U; i < 8U; i++) {
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

/** 写入 digits=6 默认配置并核对响应（config_crc16 = 0xB638） */
static void write_default_slot(uint8_t seq)
{
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, seq, WRITE_PAYLOAD_D6,
                       sizeof WRITE_PAYLOAD_D6));
    const uint8_t crc_data[2] = { 0x38U, 0xb6U };
    expect_response(PROTO_CMD_WRITE_SLOT, seq, PROTO_OK, crc_data, 2U);
}

void test_proto(void)
{
    uint8_t data[16];

    /* ---- PING：向量逐字节一致（uptime=0、flags=0） ---- */
    setup();
    CHECK(send_raw(PING_REQ, sizeof PING_REQ));
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa81000b0001000140000000000000386e");

    /* 带载荷的 PING 属载荷长度错误（错误响应只含状态字节） */
    const uint8_t one_byte[1] = { 0x00U };
    CHECK(send_request(PROTO_CMD_PING, 0x07U, one_byte, 1U));
    expect_response(PROTO_CMD_PING, 0x07U, PROTO_ERR_LEN, NULL, 0U);

    /* ---- GET_TIME：UTC 毫秒与 time_flags ---- */
    setup();
    fake_clock_rtc_set_now_ms(SET_TIME_MS);
    fake_clock_rtc_set_time_flags(CLOCK_RTC_TIME_TRUSTED);
    CHECK(send_request(PROTO_CMD_GET_TIME, 0x11U, NULL, 0U));
    put_u64le(data, SET_TIME_MS);
    data[8] = CLOCK_RTC_TIME_TRUSTED;
    expect_response(PROTO_CMD_GET_TIME, 0x11U, PROTO_OK, data, 9U);

    CHECK(send_request(PROTO_CMD_GET_TIME, 0x12U, one_byte, 1U));
    expect_response(PROTO_CMD_GET_TIME, 0x12U, PROTO_ERR_LEN, NULL, 0U);

    /* ---- SET_TIME：向量收帧并写入，响应回读一致 ---- */
    setup();
    CHECK(send_raw(SET_TIME_REQ, sizeof SET_TIME_REQ));
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa832a09000020e9f9a0010000ce41");
    CHECK(fake_clock_rtc_last_set_time_arg() == SET_TIME_MS);

    /* 长度错误、RTC 未就绪、写入失败分别回 ERR_LEN / ERR_INTERNAL / ERR_PARAM */
    uint8_t bad_time[8] = { 0 };
    CHECK(send_request(PROTO_CMD_SET_TIME, 0x20U, bad_time, 7U));
    expect_response(PROTO_CMD_SET_TIME, 0x20U, PROTO_ERR_LEN, NULL, 0U);

    setup();
    fake_clock_rtc_set_ready(false);
    CHECK(send_request(PROTO_CMD_SET_TIME, 0x21U, bad_time, 8U));
    expect_response(PROTO_CMD_SET_TIME, 0x21U, PROTO_ERR_INTERNAL, NULL, 0U);

    setup();
    fake_clock_rtc_fail_set_time(true);
    CHECK(send_request(PROTO_CMD_SET_TIME, 0x22U, bad_time, 8U));
    expect_response(PROTO_CMD_SET_TIME, 0x22U, PROTO_ERR_PARAM, NULL, 0U);

    /* ---- GET_STATUS：向量逐字节一致 ---- */
    setup();
    s_status.battery_mv = 2900U;
    CHECK(send_request(PROTO_CMD_GET_STATUS, 0x2bU, NULL, 0U));
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa842b0b00000000540b01ffffffff7c2f");

    /* flags 组装：种子 + 未对时/超期 + 低电 + 故障 + 校准 + 看门狗 */
    setup();
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x01U, WRITE_PAYLOAD_D6,
                       sizeof WRITE_PAYLOAD_D6));
    fake_clock_rtc_set_time_flags(CLOCK_RTC_TIME_NEVER_SET | CLOCK_RTC_TIME_STALE);
    fake_clock_rtc_set_calib_active(true);
    fake_clock_rtc_set_last_set_age_s(1234U);
    s_status.battery_mv = 2300U;
    s_status.battery_level = BATTERY_LEVEL_LOW;
    s_status.reset_cause = PROTO_RESET_IWDG;
    s_status.fault_latched = true;
    s_status.iwdg_enabled = true;
    const uint8_t flags_data[10] = {
        0xefU, 0x00U,                             /* 0x00EF：全部 7 位         */
        0x01U,                                    /* reset_cause = IWDG        */
        0xfcU, 0x08U,                             /* 2300 mV                   */
        0x02U,                                    /* 低电                      */
        0xd2U, 0x04U, 0x00U, 0x00U,               /* 上次对时 1234 s 前        */
    };
    CHECK(send_request(PROTO_CMD_GET_STATUS, 0x2cU, NULL, 0U));
    expect_response(PROTO_CMD_GET_STATUS, 0x2cU, PROTO_OK, flags_data, 10U);

    /* 极低电量单独映射到 CRITICAL 位 */
    setup();
    s_status.battery_level = BATTERY_LEVEL_CRITICAL;
    CHECK(send_request(PROTO_CMD_GET_STATUS, 0x2dU, NULL, 0U));
    const uint8_t critical_data[10] = { 0x10U, 0x00U, 0x00U, 0x00U, 0x00U,
                                        0x03U, 0xffU, 0xffU, 0xffU, 0xffU };
    expect_response(PROTO_CMD_GET_STATUS, 0x2dU, PROTO_OK, critical_data, 10U);

    CHECK(send_request(PROTO_CMD_GET_STATUS, 0x2eU, one_byte, 1U));
    expect_response(PROTO_CMD_GET_STATUS, 0x2eU, PROTO_ERR_LEN, NULL, 0U);

    /* ---- PING flags 与 GET_STATUS 同源 ---- */
    setup();
    write_default_slot(0x30U);
    fake_clock_rtc_set_time_flags(CLOCK_RTC_TIME_NEVER_SET);
    fake_clock_rtc_set_calib_active(true);
    CHECK(send_request(PROTO_CMD_PING, 0x31U, NULL, 0U));
    const uint8_t ping_payload[10] = {
        0x01U, 0x00U, 0x01U, 0x40U, 0x43U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    expect_response(PROTO_CMD_PING, 0x31U, PROTO_OK, ping_payload, 10U);

    /* ---- WRITE_SLOT：向量写入 + 幂等 + READ_META 回读 ---- */
    setup();
    write_default_slot(0x01U);
    CHECK(keystore_present());
    keystore_config_t cfg;
    CHECK(keystore_load(&cfg));
    CHECK(cfg.algo == 1U);
    CHECK(cfg.digits == 6U);
    CHECK(cfg.period_s == 30U);
    CHECK(cfg.secret_chars == 16U);
    CHECK(cfg.secret_len == 10U);
    CHECK_HEX(cfg.secret, cfg.secret_len, "48656c6c6f21deadbeef");
    CHECK(cfg.config_crc16 == 0xB638U);

    /* 重传相同配置不产生物理擦写（幂等要求） */
    const unsigned program_calls = fake_nvm_program_calls();
    write_default_slot(0x02U);
    CHECK(fake_nvm_program_calls() == program_calls);

    /* READ_META 响应向量（seq=2，digits=6 配置） */
    CHECK(send_request(PROTO_CMD_READ_META, 0x02U, NULL, 0U));
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa860209000301061e001038b69e9e");

    /* digits=4 的向量：配置 CRC 0xDD5E */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x03U, WRITE_PAYLOAD_D4,
                       sizeof WRITE_PAYLOAD_D4));
    const uint8_t crc_d4[2] = { 0x5eU, 0xddU };
    expect_response(PROTO_CMD_WRITE_SLOT, 0x03U, PROTO_OK, crc_d4, 2U);

    /* ---- WRITE_SLOT 参数校验 ---- */
    setup();
    uint8_t bad[21];
    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);

    bad[0] = 0x02U;                                 /* algo=2 → 不支持 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x40U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x40U, PROTO_ERR_UNSUPPORTED, NULL, 0U);

    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);
    bad[1] = 0x00U;                                 /* digits=0 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x41U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x41U, PROTO_ERR_PARAM, NULL, 0U);

    bad[1] = 0x07U;                                 /* digits=7 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x42U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x42U, PROTO_ERR_PARAM, NULL, 0U);

    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);
    bad[2] = 0x0eU;                                 /* period=14 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x43U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x43U, PROTO_ERR_PARAM, NULL, 0U);

    bad[2] = 0x2dU;                                 /* period=301 */
    bad[3] = 0x01U;
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x44U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x44U, PROTO_ERR_PARAM, NULL, 0U);

    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x45U, bad, 20U)); /* LEN=20 ≠ 5+16 */
    expect_response(PROTO_CMD_WRITE_SLOT, 0x45U, PROTO_ERR_LEN, NULL, 0U);

    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x46U, bad, 5U));   /* LEN < 6 */
    expect_response(PROTO_CMD_WRITE_SLOT, 0x46U, PROTO_ERR_LEN, NULL, 0U);

    uint8_t too_long[64] = { 0 };
    too_long[0] = 0x01U;
    too_long[1] = 0x06U;
    too_long[2] = 0x1eU;
    too_long[4] = 0x3bU;                            /* 59 字符 → LEN=64 → 拒 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x47U, too_long, 64U));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x47U, PROTO_ERR_LEN, NULL, 0U);

    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);
    bad[20] = 0x38U;                                /* '8' 不在字母表 */
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x48U, bad, (uint8_t)sizeof bad));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x48U, PROTO_ERR_PARAM, NULL, 0U);

    memcpy(bad, WRITE_PAYLOAD_D6, sizeof bad);
    bad[4] = 0x01U;                                 /* 种子改为单字符 "A"：解不出字节 */
    bad[5] = 0x41U;
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x49U, bad, 6U));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x49U, PROTO_ERR_PARAM, NULL, 0U);

    /* EEPROM 写入失败：回 ERR_EEPROM 且不留下半份配置 */
    setup();
    fake_nvm_fail_next_program(true);
    CHECK(send_request(PROTO_CMD_WRITE_SLOT, 0x4aU, WRITE_PAYLOAD_D6,
                       sizeof WRITE_PAYLOAD_D6));
    expect_response(PROTO_CMD_WRITE_SLOT, 0x4aU, PROTO_ERR_EEPROM, NULL, 0U);
    CHECK(!keystore_present());

    /* ---- READ_META：空配置是全零 + OK；带载荷回 ERR_LEN ---- */
    setup();
    CHECK(send_request(PROTO_CMD_READ_META, 0x50U, NULL, 0U));
    const uint8_t empty_meta[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    expect_response(PROTO_CMD_READ_META, 0x50U, PROTO_OK, empty_meta, 8U);

    CHECK(send_request(PROTO_CMD_READ_META, 0x51U, one_byte, 1U));
    expect_response(PROTO_CMD_READ_META, 0x51U, PROTO_ERR_LEN, NULL, 0U);

    /* ---- ERASE_SLOT：成功后转空配置；EEPROM 失败走错误响应 ---- */
    setup();
    write_default_slot(0x08U);
    CHECK(send_request(PROTO_CMD_ERASE_SLOT, 0x08U, NULL, 0U));
    expect_response(PROTO_CMD_ERASE_SLOT, 0x08U, PROTO_OK, NULL, 0U);
    CHECK(!keystore_present());

    CHECK(send_request(PROTO_CMD_ERASE_SLOT, 0x52U, one_byte, 1U));
    expect_response(PROTO_CMD_ERASE_SLOT, 0x52U, PROTO_ERR_LEN, NULL, 0U);

    setup();
    write_default_slot(0x09U);
    fake_nvm_fail_next_erase(true);
    CHECK(send_request(PROTO_CMD_ERASE_SLOT, 0x09U, NULL, 0U));
    CHECK_HEX(fake_usart2_hw_sent(), fake_usart2_hw_sent_len(),
              "aa87090106b3c5");

    /* ---- SET_CALIB：回读实际生效值；范围与失败路径 ---- */
    setup();
    const uint8_t calib[2] = { 0xd2U, 0x04U };      /* +1234（0.01 ppm） */
    CHECK(send_request(PROTO_CMD_SET_CALIB, 0x60U, calib, 2U));
    expect_response(PROTO_CMD_SET_CALIB, 0x60U, PROTO_OK, calib, 2U);
    CHECK(fake_clock_rtc_last_calib_arg() == 1234);

    const uint8_t calib_neg[2] = { 0x78U, 0xecU };  /* -5000 */
    CHECK(send_request(PROTO_CMD_SET_CALIB, 0x61U, calib_neg, 2U));
    expect_response(PROTO_CMD_SET_CALIB, 0x61U, PROTO_OK, calib_neg, 2U);

    const uint8_t calib_over[2] = { 0x31U, 0x75U }; /* +30001 → 超范围 */
    CHECK(send_request(PROTO_CMD_SET_CALIB, 0x62U, calib_over, 2U));
    expect_response(PROTO_CMD_SET_CALIB, 0x62U, PROTO_ERR_PARAM, NULL, 0U);

    CHECK(send_request(PROTO_CMD_SET_CALIB, 0x63U, calib, 1U));
    expect_response(PROTO_CMD_SET_CALIB, 0x63U, PROTO_ERR_LEN, NULL, 0U);

    setup();
    fake_clock_rtc_fail_set_calib(true);
    CHECK(send_request(PROTO_CMD_SET_CALIB, 0x64U, calib, 2U));
    expect_response(PROTO_CMD_SET_CALIB, 0x64U, PROTO_ERR_INTERNAL, NULL, 0U);

    /* ---- 可选 CW 扩展：数显设备明确拒绝，不改变种子或 RTC。 ---- */
    setup();
    write_default_slot(0x65U);
    CHECK(send_request(PROTO_CMD_GET_CW, 0x66U, NULL, 0U));
    expect_response(PROTO_CMD_GET_CW, 0x66U, PROTO_ERR_UNSUPPORTED, NULL, 0U);
    const uint8_t cw_config[3] = { 12U, 0x84U, 0x03U };
    CHECK(send_request(PROTO_CMD_SET_CW, 0x67U, cw_config, 3U));
    expect_response(PROTO_CMD_SET_CW, 0x67U, PROTO_ERR_UNSUPPORTED, NULL, 0U);
    CHECK(keystore_present());
    CHECK(fake_clock_rtc_last_calib_arg() == 0);
    CHECK(send_request(PROTO_CMD_GET_CW, 0x68U, one_byte, 1U));
    expect_response(PROTO_CMD_GET_CW, 0x68U, PROTO_ERR_LEN, NULL, 0U);
    CHECK(send_request(PROTO_CMD_SET_CW, 0x69U, cw_config, 2U));
    expect_response(PROTO_CMD_SET_CW, 0x69U, PROTO_ERR_LEN, NULL, 0U);

    /* ---- 未知命令（0x0B..0x7F 与响应码 0x80..0xFF） ---- */
    setup();
    CHECK(send_request(0x0BU, 0x70U, NULL, 0U));
    expect_response(0x0BU, 0x70U, PROTO_ERR_UNKNOWN_CMD, NULL, 0U);

    CHECK(send_request(0x80U, 0x71U, NULL, 0U));
    expect_response(0x80U, 0x71U, PROTO_ERR_UNKNOWN_CMD, NULL, 0U);

    /* ---- NULL 参数：不发送、返回 false ---- */
    setup();
    uart_link_frame_t dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.cmd = PROTO_CMD_PING;
    CHECK(!proto_handle_request(NULL, NULL, NULL));
    CHECK(!proto_handle_request(NULL, &s_status, NULL));
    CHECK(!proto_handle_request(&dummy, NULL, NULL));
    CHECK(fake_usart2_hw_sent_len() == 0U);
}
