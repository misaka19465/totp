/**
 * @file    test_base32.c
 * @brief   Base32 解码测试：RFC 4648 §10 向量 + 非法输入。
 */
#include "test_util.h"

#include "base32.h"

static void expect_decode(const char *text, const char *expected_hex)
{
    uint8_t out[64];
    const int decoded = base32_decode(text, strlen(text), out, sizeof out);

    CHECK(decoded == (int)(strlen(expected_hex) / 2U));
    if (decoded > 0) {
        CHECK_HEX(out, (size_t)decoded, expected_hex);
    }
}

void test_base32(void)
{
    /* RFC 4648 §10：MZXW6(-8) / MZXW6YTB / MZXW6YTBOI */
    expect_decode("MZXW6", "666f6f");
    expect_decode("MZXW6YTB", "666f6f6261");
    expect_decode("MZXW6YTBOI", "666f6f626172");

    /* 协议示例种子：Hello!\xde\xad\xbe\xef */
    expect_decode("JBSWY3DPEHPK3PXP", "48656c6c6f21deadbeef");

    /* 最小有效输入：2 字符 → 1 字节 */
    expect_decode("AA", "00");

    uint8_t out[8];

    /* 非法输入 */
    CHECK(base32_decode("mZXW6", 5U, out, sizeof out) == -1);   /* 小写 */
    CHECK(base32_decode("MZXW6=", 6U, out, sizeof out) == -1);  /* 填充 */
    CHECK(base32_decode("MZXW!", 5U, out, sizeof out) == -1);   /* 非法字符 */
    CHECK(base32_decode("", 0U, out, sizeof out) == -1);        /* 空串 */
    CHECK(base32_decode("A", 1U, out, sizeof out) == -1);       /* 只含填充位，0 字节 */
    CHECK(base32_decode("MZ", 2U, out, sizeof out) == -1);      /* 尾部残留位非 0 */
    CHECK(base32_decode("MZXW6YTB", 8U, out, 1U) == -1);        /* 容量不足 */
}
