/**
 * @file    test_totp.c
 * @brief   HOTP/TOTP 测试：RFC 4226 附录 D + RFC 6238 附录 B（SHA-1）。
 */
#include "test_util.h"

#include "totp.h"

/* RFC 4226/6238 的 SHA-1 测试密钥：ASCII "12345678901234567890" */
static const uint8_t RFC_SECRET[20] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
};

void test_totp(void)
{
    /* RFC 4226 附录 D：计数器 0..9 对应的 6 位 HOTP */
    static const uint32_t hotp_vectors[10] = {
        755224U, 287082U, 359152U, 969429U, 338314U,
        254676U, 287922U, 162583U, 399871U, 520489U,
    };
    for (uint64_t counter = 0U; counter < 10U; counter++) {
        uint32_t truncated = 0U;
        CHECK(hotp_generate(RFC_SECRET, sizeof RFC_SECRET, counter, &truncated));
        CHECK_U32(truncated % 1000000U, hotp_vectors[counter]);

        uint32_t code = 0U;
        CHECK(totp_generate(RFC_SECRET, sizeof RFC_SECRET,
                            counter * 30U, 30U, 6U, &code));
        CHECK_U32(code, hotp_vectors[counter]);
    }

    /* RFC 6238 附录 B：SHA-1，8 位口令 */
    static const struct {
        uint64_t unix_seconds;
        uint32_t code;
    } totp_vectors[] = {
        {59U,          94287082U},
        {1111111109U,  7081804U},   /* RFC 向量 07081804，数值即 7081804 */
        {1111111111U,  14050471U},
        {1234567890U,  89005924U},
        {2000000000U,  69279037U},
        {20000000000U, 65353130U},
    };
    for (size_t i = 0U; i < sizeof totp_vectors / sizeof totp_vectors[0]; i++) {
        uint32_t code = 0U;
        CHECK(totp_generate(RFC_SECRET, sizeof RFC_SECRET,
                            totp_vectors[i].unix_seconds, 30U, 8U, &code));
        CHECK_U32(code, totp_vectors[i].code);
    }

    /* 短位数 = 完整值对 10^digits 取模 */
    uint32_t code = 0U;
    CHECK(totp_generate(RFC_SECRET, sizeof RFC_SECRET, 59U, 30U, 6U, &code));
    CHECK_U32(code, 287082U);
    CHECK(totp_generate(RFC_SECRET, sizeof RFC_SECRET, 59U, 30U, 4U, &code));
    CHECK_U32(code, 7082U);

    /* 参数校验：非法输入必须拒绝且不写输出 */
    code = 0xFFFFFFFFU;
    CHECK(!totp_generate(RFC_SECRET, sizeof RFC_SECRET, 59U, 30U, 0U, &code));
    CHECK(!totp_generate(RFC_SECRET, sizeof RFC_SECRET, 59U, 30U, 9U, &code));
    CHECK(!totp_generate(RFC_SECRET, sizeof RFC_SECRET, 59U, 0U, 6U, &code));
    CHECK(!totp_generate(NULL, 0U, 59U, 30U, 6U, &code));
    CHECK(!totp_generate(RFC_SECRET, 0U, 59U, 30U, 6U, &code));
    CHECK(!hotp_generate(RFC_SECRET, sizeof RFC_SECRET, 0U, NULL));
    CHECK_U32(code, 0xFFFFFFFFU);

    /* 剩余秒数 */
    CHECK_U32(totp_seconds_remaining(59U, 30U), 1U);
    CHECK_U32(totp_seconds_remaining(60U, 30U), 30U);
    CHECK_U32(totp_seconds_remaining(0U, 30U), 30U);
    CHECK_U32(totp_seconds_remaining(89U, 30U), 1U);
    CHECK_U32(totp_seconds_remaining(100U, 0U), 0U);
}
