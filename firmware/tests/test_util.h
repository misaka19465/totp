/**
 * @file    test_util.h
 * @brief   极简测试断言宏（无外部框架，主机侧运行）。
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned g_checks;
extern unsigned g_failures;

static inline void test_print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static inline void test_hex_to_bytes(const char *hex, uint8_t *out)
{
    for (size_t i = 0U; i < strlen(hex) / 2U; i++) {
        unsigned value = 0U;
        (void)sscanf(&hex[i * 2U], "%2x", &value);
        out[i] = (uint8_t)value;
    }
}

/** 布尔断言 */
#define CHECK(cond) do {                                                    \
    g_checks++;                                                             \
    if (!(cond)) {                                                          \
        g_failures++;                                                       \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                       \
} while (0)

/** u32 相等断言（打印十六进制便于对照） */
#define CHECK_U32(actual, expected) do {                                    \
    const uint32_t check_a_ = (uint32_t)(actual);                           \
    const uint32_t check_e_ = (uint32_t)(expected);                         \
    g_checks++;                                                             \
    if (check_a_ != check_e_) {                                             \
        g_failures++;                                                       \
        printf("FAIL %s:%d: %s = 0x%08X，期望 0x%08X\n",                    \
               __FILE__, __LINE__, #actual, check_a_, check_e_);            \
    }                                                                       \
} while (0)

/** 字节串与十六进制期望值比对 */
#define CHECK_HEX(actual, actual_len, expected_hex) do {                    \
    uint8_t check_exp_[32];                                                 \
    const size_t check_len_ = strlen(expected_hex) / 2U;                    \
    test_hex_to_bytes(expected_hex, check_exp_);                            \
    g_checks++;                                                             \
    if ((actual_len) != check_len_ ||                                       \
        memcmp((actual), check_exp_, check_len_) != 0) {                    \
        g_failures++;                                                       \
        printf("FAIL %s:%d: %s = ", __FILE__, __LINE__, #actual);           \
        test_print_hex((const uint8_t *)(actual), (size_t)(actual_len));    \
        printf("，期望 %s\n", expected_hex);                                \
    }                                                                       \
} while (0)

#endif /* TEST_UTIL_H */
