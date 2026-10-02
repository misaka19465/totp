/**
 * @file    test_hmac_sha1.c
 * @brief   HMAC-SHA1 测试：RFC 2202 §3 向量（含 >64 B 长密钥分支）。
 */
#include "test_util.h"

#include "hmac_sha1.h"

void test_hmac_sha1(void)
{
    uint8_t mac[HMAC_SHA1_DIGEST_LEN];

    /* 用例 1：20 × 0x0b，"Hi There" */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof key1);
    hmac_sha1(key1, sizeof key1, (const uint8_t *)"Hi There", 8U, mac);
    CHECK_HEX(mac, sizeof mac, "b617318655057264e28bc0b6fb378c8ef146be00");

    /* 用例 2："Jefe" / "what do ya want for nothing?" */
    hmac_sha1((const uint8_t *)"Jefe", 4U,
              (const uint8_t *)"what do ya want for nothing?", 28U, mac);
    CHECK_HEX(mac, sizeof mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

    /* 用例 3：20 × 0xaa 密钥，50 × 0xdd 数据 */
    uint8_t key3[20];
    uint8_t data3[50];
    memset(key3, 0xaa, sizeof key3);
    memset(data3, 0xdd, sizeof data3);
    hmac_sha1(key3, sizeof key3, data3, sizeof data3, mac);
    CHECK_HEX(mac, sizeof mac, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");

    /* 用例 4：0x01..0x19（25 B）密钥，50 × 0xcd 数据 */
    uint8_t key4[25];
    uint8_t data4[50];
    for (uint8_t i = 0U; i < sizeof key4; i++) {
        key4[i] = (uint8_t)(i + 1U);
    }
    memset(data4, 0xcd, sizeof data4);
    hmac_sha1(key4, sizeof key4, data4, sizeof data4, mac);
    CHECK_HEX(mac, sizeof mac, "4c9007f4026250c6bc8414f9bf50c86c2d7235da");

    /* 用例 5：20 × 0x0c，"Test With Truncation"（20 字节） */
    uint8_t key5[20];
    memset(key5, 0x0c, sizeof key5);
    hmac_sha1(key5, sizeof key5, (const uint8_t *)"Test With Truncation", 20U, mac);
    CHECK_HEX(mac, sizeof mac, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");

    /* 用例 6：80 × 0xaa，长于分块，走 SHA-1 压缩分支 */
    uint8_t key6[80];
    memset(key6, 0xaa, sizeof key6);
    hmac_sha1(key6, sizeof key6,
              (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First",
              54U, mac);
    CHECK_HEX(mac, sizeof mac, "aa4ae5e15272d00e95705637ce8a3b55ed402112");

    /* 用例 7：80 × 0xaa，消息跨多个 SHA-1 分块 */
    hmac_sha1(key6, sizeof key6,
              (const uint8_t *)"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data",
              73U, mac);
    CHECK_HEX(mac, sizeof mac, "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
}
