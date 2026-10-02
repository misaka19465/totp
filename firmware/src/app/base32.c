/**
 * @file    base32.c
 * @brief   Base32 种子解码实现（RFC 4648 字母表，无填充）。
 */
#include "base32.h"

/** 返回字符的 5 bit 值；非字母表字符返回 -1 */
static int8_t decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (int8_t)(c - 'A');
    }
    if (c >= '2' && c <= '7') {
        return (int8_t)(c - '2' + 26);
    }
    return -1;
}

int base32_decode(const char *text, size_t text_len, uint8_t *out, size_t out_cap)
{
    if (text == NULL || out == NULL) {
        return -1;
    }
    if (text_len < BASE32_MIN_CHARS || text_len > BASE32_MAX_CHARS) {
        return -1;
    }

    const size_t out_len = (text_len * 5U) / 8U;
    if (out_len == 0U || out_len > out_cap || out_len > BASE32_MAX_BYTES) {
        return -1;
    }

    /* NOTE: 5 bit/字符 与 8 bit/字节 不整除，尾部残留 1..7 bit。
     *       规范编码要求这些位为 0，校验可拒绝同一字节串的另一种写法。 */
    const size_t leftover_bits = (text_len * 5U) % 8U;
    if (leftover_bits != 0U) {
        const int8_t last = decode_char(text[text_len - 1U]);
        if (last < 0) {
            return -1;
        }
        const uint8_t leftover_mask = (uint8_t)((1U << leftover_bits) - 1U);
        if (((uint8_t)last & leftover_mask) != 0U) {
            return -1;
        }
    }

    uint32_t acc = 0U;
    size_t acc_bits = 0U;
    size_t out_index = 0U;
    for (size_t i = 0U; i < text_len; i++) {
        const int8_t value = decode_char(text[i]);
        if (value < 0) {
            return -1;
        }
        acc = (acc << 5U) | (uint32_t)value;
        acc_bits += 5U;
        if (acc_bits >= 8U) {
            acc_bits -= 8U;
            out[out_index] = (uint8_t)((acc >> acc_bits) & 0xFFU);
            out_index++;
        }
    }

    return (int)out_index;
}
