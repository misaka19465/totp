/**
 * @file    totp.c
 * @brief   HOTP/TOTP 实现（RFC 4226 / RFC 6238，HMAC-SHA1）。
 */
#include "totp.h"

#include "hmac_sha1.h"

/* 10^0 .. 10^8，与 TOTP_DIGITS_MAX 对应 */
static const uint32_t POW10[TOTP_DIGITS_MAX + 1U] = {
    1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U, 100000000U,
};

bool hotp_generate(const uint8_t *secret, size_t secret_len,
                   uint64_t counter, uint32_t *truncated_out)
{
    if (secret == NULL || secret_len == 0U || truncated_out == NULL) {
        return false;
    }

    /* RFC 4226 §5.2：计数器按大端 8 字节拼进消息 */
    uint8_t message[8];
    for (uint8_t i = 0U; i < 8U; i++) {
        message[i] = (uint8_t)(counter >> (8U * (7U - i)));
    }

    uint8_t mac[HMAC_SHA1_DIGEST_LEN];
    hmac_sha1(secret, secret_len, message, sizeof message, mac);

    /* RFC 4226 §5.3 动态截断：低 4 bit 选偏移，取 4 字节、清符号位 */
    const uint8_t offset = (uint8_t)(mac[HMAC_SHA1_DIGEST_LEN - 1U] & 0x0FU);
    const uint32_t truncated =
        ((uint32_t)(mac[offset] & 0x7FU) << 24U) |
        ((uint32_t)mac[offset + 1U] << 16U) |
        ((uint32_t)mac[offset + 2U] << 8U) |
        (uint32_t)mac[offset + 3U];

    *truncated_out = truncated;
    return true;
}

bool totp_generate(const uint8_t *secret, size_t secret_len,
                   uint64_t unix_seconds, uint32_t period_s,
                   uint8_t digits, uint32_t *code_out)
{
    if (period_s == 0U || code_out == NULL ||
        digits < TOTP_DIGITS_MIN || digits > TOTP_DIGITS_MAX) {
        return false;
    }

    uint32_t truncated = 0U;
    if (!hotp_generate(secret, secret_len, unix_seconds / period_s, &truncated)) {
        return false;
    }

    *code_out = truncated % POW10[digits];
    return true;
}

uint32_t totp_seconds_remaining(uint64_t unix_seconds, uint32_t period_s)
{
    if (period_s == 0U) {
        return 0U;
    }
    const uint32_t elapsed = (uint32_t)(unix_seconds % period_s);
    return period_s - elapsed;
}

uint64_t totp_next_boundary_ms(uint64_t unix_ms, uint32_t period_s)
{
    if (period_s == 0U) {
        return 0U;
    }
    const uint64_t now_s = unix_ms / 1000U;
    /* totp_seconds_remaining 至少为 1，因此边界严格晚于当前秒；
     * 当前秒内的毫秒部分被舍去，边界落在整秒上。 */
    const uint32_t remaining = totp_seconds_remaining(now_s, period_s);
    return (now_s + remaining) * 1000U;
}
