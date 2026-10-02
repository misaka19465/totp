/**
 * @file    hmac_sha1.c
 * @brief   HMAC-SHA1 实现：内含私有 SHA-1 核心（FIPS 180-1）。
 *
 * NOTE: SHA-1 只作为 HMAC 的内部原语对外暴露，本文件不提供裸 SHA-1 API，
 *       避免后续被人误用到需要抗碰撞的场合（本设备也不需要）。
 * NOTE: 实现按"小端字 + 大端字节序"的标准结构书写，字节序与 MCU 无关。
 */
#include "hmac_sha1.h"

#include <string.h>

#define SHA1_BLOCK_LEN   64U
#define SHA1_DIGEST_LEN  20U

#define ROL32(value, bits) (((value) << (bits)) | ((value) >> (32U - (bits))))

typedef struct {
    uint32_t h[5];
    uint64_t total_bytes;
    uint8_t  block[SHA1_BLOCK_LEN];
    size_t   block_len;
} sha1_ctx_t;

static void sha1_process_block(sha1_ctx_t *ctx, const uint8_t block[SHA1_BLOCK_LEN])
{
    uint32_t w[80];

    for (uint8_t i = 0U; i < 16U; i++) {
        w[i] = ((uint32_t)block[i * 4U] << 24U) |
               ((uint32_t)block[i * 4U + 1U] << 16U) |
               ((uint32_t)block[i * 4U + 2U] << 8U) |
               (uint32_t)block[i * 4U + 3U];
    }
    for (uint8_t i = 16U; i < 80U; i++) {
        w[i] = ROL32(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];

    for (uint8_t i = 0U; i < 80U; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20U) {
            f = (b & c) | (~b & d);
            k = 0x5A827999U;
        } else if (i < 40U) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60U) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        const uint32_t temp = ROL32(a, 5U) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30U);
        b = a;
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->h[0] = 0x67452301U;
    ctx->h[1] = 0xEFCDAB89U;
    ctx->h[2] = 0x98BADCFEU;
    ctx->h[3] = 0x10325476U;
    ctx->h[4] = 0xC3D2E1F0U;
    ctx->total_bytes = 0U;
    ctx->block_len = 0U;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (len == 0U) {
        return;
    }
    ctx->total_bytes += (uint64_t)len;

    while (len > 0U) {
        const size_t space = SHA1_BLOCK_LEN - ctx->block_len;
        const size_t take = (len < space) ? len : space;
        memcpy(&ctx->block[ctx->block_len], data, take);
        ctx->block_len += take;
        data += take;
        len -= take;

        if (ctx->block_len == SHA1_BLOCK_LEN) {
            sha1_process_block(ctx, ctx->block);
            ctx->block_len = 0U;
        }
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_LEN])
{
    const uint64_t total_bits = ctx->total_bytes << 3U;

    /* 填充：0x80 + 若干 0x00，使 (字节数 mod 64) == 56，再写 64 bit 大端长度 */
    const uint8_t one = 0x80U;
    sha1_update(ctx, &one, 1U);
    const uint8_t zero = 0x00U;
    while (ctx->block_len != SHA1_BLOCK_LEN - 8U) {
        sha1_update(ctx, &zero, 1U);
    }

    uint8_t length_be[8];
    for (uint8_t i = 0U; i < 8U; i++) {
        length_be[i] = (uint8_t)(total_bits >> (8U * (7U - i)));
    }
    sha1_update(ctx, length_be, sizeof length_be);

    for (uint8_t i = 0U; i < 5U; i++) {
        out[i * 4U]      = (uint8_t)(ctx->h[i] >> 24U);
        out[i * 4U + 1U] = (uint8_t)(ctx->h[i] >> 16U);
        out[i * 4U + 2U] = (uint8_t)(ctx->h[i] >> 8U);
        out[i * 4U + 3U] = (uint8_t)ctx->h[i];
    }
}

void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *data, size_t data_len,
               uint8_t out[HMAC_SHA1_DIGEST_LEN])
{
    uint8_t key_block[HMAC_SHA1_BLOCK_LEN] = {0};
    uint8_t inner[HMAC_SHA1_DIGEST_LEN];
    uint8_t pad[HMAC_SHA1_BLOCK_LEN];
    sha1_ctx_t ctx;

    if (key_len > HMAC_SHA1_BLOCK_LEN) {
        sha1_init(&ctx);
        sha1_update(&ctx, key, key_len);
        sha1_final(&ctx, key_block);
    } else if (key_len > 0U) {
        memcpy(key_block, key, key_len);
    }

    for (uint8_t i = 0U; i < HMAC_SHA1_BLOCK_LEN; i++) {
        pad[i] = (uint8_t)(key_block[i] ^ 0x36U);
    }
    sha1_init(&ctx);
    sha1_update(&ctx, pad, sizeof pad);
    sha1_update(&ctx, data, data_len);
    sha1_final(&ctx, inner);

    for (uint8_t i = 0U; i < HMAC_SHA1_BLOCK_LEN; i++) {
        pad[i] = (uint8_t)(key_block[i] ^ 0x5CU);
    }
    sha1_init(&ctx);
    sha1_update(&ctx, pad, sizeof pad);
    sha1_update(&ctx, inner, sizeof inner);
    sha1_final(&ctx, out);
}
