/**
 * @file    hmac_sha1.h
 * @brief   HMAC-SHA1（RFC 2104 + FIPS 180-1），TOTP 的唯一哈希原语。
 *
 * NOTE: 这是产品功能的核心计算，但不承担安全职责（requirements.md §1）：
 *       选 SHA-1 是因为 TOTP/RFC 6238 的默认算法是它，校验端工具链成熟。
 */
#ifndef HMAC_SHA1_H
#define HMAC_SHA1_H

#include <stddef.h>
#include <stdint.h>

/** HMAC-SHA1 摘要长度（字节） */
#define HMAC_SHA1_DIGEST_LEN   20U
/** HMAC 的 SHA-1 分块长度（字节） */
#define HMAC_SHA1_BLOCK_LEN    64U

/**
 * @brief  计算 HMAC-SHA1 摘要。
 * @param  key      密钥；key_len > 64 时先按 RFC 2104 做 SHA-1 压缩。
 * @param  key_len  密钥长度（字节），可为 0。
 * @param  data     消息，可为 NULL 当 data_len == 0。
 * @param  data_len 消息长度（字节）。
 * @param  out      输出缓冲区，须至少 HMAC_SHA1_DIGEST_LEN 字节可写。
 */
void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               uint8_t out[HMAC_SHA1_DIGEST_LEN]);

#endif /* HMAC_SHA1_H */
