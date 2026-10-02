/**
 * @file    proto.c
 * @brief   耳机口协议命令层实现（serial-protocol.md §5）。
 */
#include "proto.h"

#include <string.h>

#include "base32.h"
#include "clock_rtc.h"
#include "crc16.h"
#include "keystore.h"

/* ---- 小端字段读写（§2.2：所有多字节字段小端） ---- */

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t value = 0U;
    for (uint8_t i = 8U; i > 0U; i--) {
        value = (value << 8) | p[i - 1U];
    }
    return value;
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (uint8_t i = 0U; i < 4U; i++) {
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

static void put_u64(uint8_t *p, uint64_t value)
{
    for (uint8_t i = 0U; i < 8U; i++) {
        p[i] = (uint8_t)(value >> (8U * i));
    }
}

/* ---- 状态快照与标志组装 ---- */

/** 电量档位 → 线上取值（§5.5：0=未知，1=正常，2=低电，3=极低） */
static uint8_t battery_level_wire(battery_level_t level)
{
    switch (level) {
    case BATTERY_LEVEL_NORMAL:   return 1U;
    case BATTERY_LEVEL_LOW:      return 2U;
    case BATTERY_LEVEL_CRITICAL: return 3U;
    case BATTERY_LEVEL_UNKNOWN:  return 0U;
    }
    return 0U;      /* 不可达；仅为满足穷尽返回 */
}

/** 组装 PING/GET_STATUS 共用的 flags（§5.10） */
static uint16_t device_flags(const proto_status_t *status)
{
    uint16_t flags = 0U;

    if (keystore_present()) {
        flags |= PROTO_FLAG_SEED_PRESENT;
    }

    const uint8_t time_flags = clock_rtc_time_flags();
    if ((time_flags & CLOCK_RTC_TIME_NEVER_SET) != 0U) {
        flags |= PROTO_FLAG_RTC_NEVER_SET;
    }
    if ((time_flags & CLOCK_RTC_TIME_STALE) != 0U) {
        flags |= PROTO_FLAG_RTC_STALE;
    }

    if (status->battery_level == BATTERY_LEVEL_LOW) {
        flags |= PROTO_FLAG_LOW_BATTERY;
    }
    if (status->battery_level == BATTERY_LEVEL_CRITICAL) {
        flags |= PROTO_FLAG_CRITICAL_BATTERY;
    }

    if (status->fault_latched) {
        flags |= PROTO_FLAG_FAULT_LATCHED;
    }
    if (clock_rtc_calib_active()) {
        flags |= PROTO_FLAG_CALIB_ACTIVE;
    }
    if (status->iwdg_enabled) {
        flags |= PROTO_FLAG_IWDG_ENABLED;
    }

    return flags;
}

/** 发送仅带状态码的响应（长度为 0 时 data 可为 NULL） */
/* NOTE: 结果码经文件级变量回传给 proto_handle_request 的 result_out：
 *       协议是严格的单请求串行模型（§4.5），不存在并发/reentrancy。 */
static proto_result_t s_last_result = PROTO_ERR_INTERNAL;

static bool respond(const uart_link_frame_t *request, proto_result_t result,
                    const uint8_t *data, uint8_t data_len)
{
    s_last_result = result;
    return uart_link_send_response(request, (uint8_t)result, data, data_len);
}

/* ---- 各命令处理 ---- */

/** PING（§5.2）：连通性与版本握手，无载荷 */
static bool handle_ping(const uart_link_frame_t *request, const proto_status_t *status)
{
    if (request->len != 0U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }

    uint8_t data[10];
    data[0] = PROTO_VERSION;
    put_u16(&data[1], PROTO_FW_VERSION);
    data[3] = (uint8_t)PROTO_MAX_PAYLOAD;
    put_u16(&data[4], device_flags(status));
    put_u32(&data[6], status->uptime_s);
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** GET_TIME（§5.3）：当前 UTC 毫秒与可信标志 */
static bool handle_get_time(const uart_link_frame_t *request)
{
    if (request->len != 0U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }

    uint8_t data[9];
    put_u64(data, clock_rtc_now_ms());
    data[8] = clock_rtc_time_flags();
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** SET_TIME（§5.4）：写 UTC 毫秒并回读确认 */
static bool handle_set_time(const uart_link_frame_t *request)
{
    if (request->len != 8U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }
    if (!clock_rtc_ready()) {
        return respond(request, PROTO_ERR_INTERNAL, NULL, 0U);
    }

    const uint64_t target_ms = get_u64(request->payload);
    uint64_t readback_ms = 0U;
    if (!clock_rtc_set_unix_ms(target_ms, &readback_ms)) {
        /* 唯一可预期的失败是时间戳超出 RTC 可表达的 2000..2099 年 */
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }

    uint8_t data[8];
    put_u64(data, readback_ms);
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** GET_STATUS（§5.5）：状态全量快照 */
static bool handle_get_status(const uart_link_frame_t *request,
                              const proto_status_t *status)
{
    if (request->len != 0U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }

    uint8_t data[10];
    put_u16(&data[0], device_flags(status));
    data[2] = status->reset_cause;
    put_u16(&data[3], status->battery_mv);
    data[5] = battery_level_wire(status->battery_level);
    put_u32(&data[6], clock_rtc_last_set_age_s());
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** WRITE_SLOT（§5.6）：校验 → 解码 → 落盘（含读回比对） */
static bool handle_write_slot(const uart_link_frame_t *request)
{
    const uint8_t *payload = request->payload;

    if (request->len < 6U || request->len > 61U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }
    if ((uint8_t)(payload[4] + 5U) != request->len) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }
    if (payload[0] != KEYSTORE_ALGO_SHA1) {
        return respond(request, PROTO_ERR_UNSUPPORTED, NULL, 0U);
    }
    if (payload[1] < KEYSTORE_DIGITS_MIN || payload[1] > KEYSTORE_DIGITS_MAX) {
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }

    const uint16_t period_s = get_u16(&payload[2]);
    if (period_s < KEYSTORE_PERIOD_MIN_S || period_s > KEYSTORE_PERIOD_MAX_S) {
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }
    if (payload[4] < KEYSTORE_SECRET_CHARS_MIN ||
        payload[4] > KEYSTORE_SECRET_CHARS_MAX) {
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }

    keystore_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.algo = payload[0];
    cfg.digits = payload[1];
    cfg.period_s = period_s;
    cfg.secret_chars = payload[4];
    cfg.config_crc16 = crc16_ccitt_false(payload, (size_t)payload[4] + 5U);

    const int decoded = base32_decode((const char *)&payload[5], payload[4],
                                      cfg.secret, sizeof cfg.secret);
    if (decoded < 0) {
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }
    cfg.secret_len = (uint8_t)decoded;

    if (!keystore_write(&cfg)) {
        return respond(request, PROTO_ERR_EEPROM, NULL, 0U);
    }

    uint8_t data[2];
    put_u16(data, cfg.config_crc16);
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** READ_META（§5.7）：配置元信息与 config_crc16，不回显种子 */
static bool handle_read_meta(const uart_link_frame_t *request)
{
    if (request->len != 0U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }

    uint8_t data[8] = { 0 };
    keystore_config_t cfg;
    if (keystore_load(&cfg)) {
        data[0] = PROTO_META_FLAG_SEED_PRESENT | PROTO_META_FLAG_RECORD_VALID;
        data[1] = cfg.algo;
        data[2] = cfg.digits;
        put_u16(&data[3], cfg.period_s);
        data[5] = cfg.secret_chars;
        put_u16(&data[6], cfg.config_crc16);
    }
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

/** ERASE_SLOT（§5.8）：使配置记录失效 */
static bool handle_erase_slot(const uart_link_frame_t *request)
{
    if (request->len != 0U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }
    if (!keystore_erase()) {
        return respond(request, PROTO_ERR_EEPROM, NULL, 0U);
    }
    return respond(request, PROTO_OK, NULL, 0U);
}

/** SET_CALIB（§5.8）：写 RTC 数字校准并回读实际生效值 */
static bool handle_set_calib(const uart_link_frame_t *request)
{
    if (request->len != 2U) {
        return respond(request, PROTO_ERR_LEN, NULL, 0U);
    }

    const int16_t requested = (int16_t)get_u16(request->payload);
    if (requested < -PROTO_CAL_CENTI_PPM_MAX || requested > PROTO_CAL_CENTI_PPM_MAX) {
        return respond(request, PROTO_ERR_PARAM, NULL, 0U);
    }

    int16_t applied = 0;
    if (!clock_rtc_set_calib(requested, &applied)) {
        return respond(request, PROTO_ERR_INTERNAL, NULL, 0U);
    }

    uint8_t data[2];
    put_u16(data, (uint16_t)applied);
    return respond(request, PROTO_OK, data, (uint8_t)sizeof data);
}

bool proto_handle_request(const uart_link_frame_t *request,
                          const proto_status_t *status,
                          proto_result_t *result_out)
{
    if (request == NULL || status == NULL) {
        return false;
    }

    s_last_result = PROTO_ERR_INTERNAL;
    bool sent;
    switch (request->cmd) {
    case PROTO_CMD_PING:       sent = handle_ping(request, status); break;
    case PROTO_CMD_GET_TIME:   sent = handle_get_time(request); break;
    case PROTO_CMD_SET_TIME:   sent = handle_set_time(request); break;
    case PROTO_CMD_GET_STATUS: sent = handle_get_status(request, status); break;
    case PROTO_CMD_WRITE_SLOT: sent = handle_write_slot(request); break;
    case PROTO_CMD_READ_META:  sent = handle_read_meta(request); break;
    case PROTO_CMD_ERASE_SLOT: sent = handle_erase_slot(request); break;
    case PROTO_CMD_SET_CALIB:  sent = handle_set_calib(request); break;
    default:
        sent = respond(request, PROTO_ERR_UNKNOWN_CMD, NULL, 0U);
        break;
    }
    if (result_out != NULL) {
        *result_out = s_last_result;
    }
    return sent;
}
