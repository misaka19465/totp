/**
 * @file    test_keystore.c
 * @brief   keystore 单元测试：记录往返、幂等、参数校验、损坏检测与失败回滚。
 *
 * config_crc16 用 serial-protocol.md §5.7 的定义独立复算，并与 §8 的
 * 测试向量（JBSWY3DPEHPK3PXP / digits=6 → 0xB638，digits=4 → 0xDD5E）
 * 逐值对照，保证存储层与协议参考实现一致。
 */
#include "test_util.h"

#include <string.h>

#include "base32.h"
#include "crc16.h"
#include "fake_nvm.h"
#include "keystore.h"

#define SEED_TEXT      "JBSWY3DPEHPK3PXP"
#define SEED_TEXT_LEN  16U

/** serial-protocol.md §5.7：CRC-16/CCITT-FALSE(algo||digits||period_le||len||ascii) */
static uint16_t spec_config_crc(uint8_t algo, uint8_t digits, uint16_t period,
                                const char *secret, uint8_t secret_len)
{
    uint8_t blob[5U + BASE32_MAX_CHARS];
    blob[0] = algo;
    blob[1] = digits;
    blob[2] = (uint8_t)(period & 0xFFU);
    blob[3] = (uint8_t)(period >> 8);
    blob[4] = secret_len;
    memcpy(&blob[5], secret, secret_len);
    return crc16_ccitt_false(blob, 5U + secret_len);
}

/** 用协议向量种子构造配置：验证解码由调用方完成并存入 10 字节密钥 */
static bool make_config(keystore_config_t *cfg, uint8_t digits, uint16_t config_crc)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->algo = KEYSTORE_ALGO_SHA1;
    cfg->digits = digits;
    cfg->period_s = 30U;
    cfg->config_crc16 = config_crc;
    const int decoded = base32_decode(SEED_TEXT, SEED_TEXT_LEN,
                                      cfg->secret, sizeof cfg->secret);
    cfg->secret_len = (uint8_t)decoded;
    return decoded == 10;
}

static bool record_words_are_zero(void)
{
    const uint32_t *words = fake_nvm_words();
    for (uint16_t i = 0U; i < KEYSTORE_RECORD_WORDS; i++) {
        if (words[i] != 0U) {
            return false;
        }
    }
    return true;
}

void test_keystore(void)
{
    keystore_config_t cfg;
    keystore_config_t back;
    fake_nvm_reset();

    /* ---- 空 EEPROM：无有效记录，擦除幂等 ---- */
    CHECK(!keystore_present());
    CHECK(!keystore_load(&cfg));
    CHECK(keystore_erase());
    CHECK(!keystore_present());

    /* ---- CRC 与规范 §8 向量一致 ---- */
    CHECK_U32(spec_config_crc(1U, 6U, 30U, SEED_TEXT, SEED_TEXT_LEN), 0xB638U);
    CHECK_U32(spec_config_crc(1U, 4U, 30U, SEED_TEXT, SEED_TEXT_LEN), 0xDD5EU);

    /* ---- 写入 → 读回（digits=6，种子解码为 Hello!\xDE\xAD\xBE\xEF） ---- */
    CHECK(make_config(&cfg, 6U, 0xB638U));
    CHECK_HEX(cfg.secret, cfg.secret_len, "48656c6c6f21deadbeef");
    CHECK(keystore_write(&cfg));
    CHECK(keystore_present());
    CHECK(keystore_load(&back));
    CHECK(back.algo == KEYSTORE_ALGO_SHA1);
    CHECK(back.digits == 6U);
    CHECK(back.period_s == 30U);
    CHECK(back.secret_len == 10U);
    CHECK(back.config_crc16 == 0xB638U);
    CHECK(memcmp(back.secret, cfg.secret, 10U) == 0);

    /* 记录只占 13 个字，其余字保持未写入态 */
    CHECK(fake_nvm_words()[KEYSTORE_RECORD_WORDS] == 0U);

    /* ---- 幂等：相同配置不产生物理擦写 ---- */
    const unsigned program_calls = fake_nvm_program_calls();
    const unsigned erase_calls = fake_nvm_erase_calls();
    CHECK(keystore_write(&cfg));
    CHECK(fake_nvm_program_calls() == program_calls);
    CHECK(fake_nvm_erase_calls() == erase_calls);

    /* ---- 参数越界拒绝，且不破坏已有记录 ---- */
    struct {
        uint8_t algo;
        uint8_t digits;
        uint16_t period;
        uint8_t secret_len;
    } const bad[] = {
        { 0U, 6U, 30U,  10U },   /* algo=0          */
        { 2U, 6U, 30U,  10U },   /* algo=2          */
        { 1U, 0U, 30U,  10U },   /* digits=0        */
        { 1U, 7U, 30U,  10U },   /* digits=7        */
        { 1U, 6U, 14U,  10U },   /* period=14       */
        { 1U, 6U, 301U, 10U },   /* period=301      */
        { 1U, 6U, 30U,  0U  },   /* secret_len=0    */
        { 1U, 6U, 30U,  36U },   /* secret_len=36   */
    };
    for (size_t i = 0U; i < sizeof bad / sizeof bad[0]; i++) {
        keystore_config_t bad_cfg = cfg;
        bad_cfg.algo = bad[i].algo;
        bad_cfg.digits = bad[i].digits;
        bad_cfg.period_s = bad[i].period;
        bad_cfg.secret_len = bad[i].secret_len;
        CHECK(!keystore_write(&bad_cfg));
        CHECK(keystore_load(&back));
        CHECK(back.digits == 6U);           /* 原记录未被触碰 */
    }
    CHECK(!keystore_write(NULL));

    /* ---- 换配置（digits=4）真的落盘 ---- */
    CHECK(make_config(&cfg, 4U, 0xDD5EU));
    CHECK(keystore_write(&cfg));
    CHECK(keystore_load(&back));
    CHECK(back.digits == 4U);
    CHECK(back.config_crc16 == 0xDD5EU);
    CHECK(fake_nvm_program_calls() > program_calls);

    /* ---- 记录被损坏 → 视为未写入种子 ---- */
    fake_nvm_words()[2] ^= 0x00010000U;     /* 翻转记录 CRC 覆盖区内的一位 */
    CHECK(!keystore_present());
    CHECK(make_config(&cfg, 6U, 0xB638U));
    CHECK(keystore_write(&cfg));            /* 重写即可恢复 */
    CHECK(keystore_load(&back));
    CHECK(back.digits == 6U);

    /* ---- 写入中途失败 → 回滚擦除，不留半份配置 ---- */
    CHECK(make_config(&cfg, 4U, 0xDD5EU));
    fake_nvm_fail_next_program(true);
    CHECK(!keystore_write(&cfg));
    CHECK(!keystore_present());
    CHECK(record_words_are_zero());

    /* ---- 擦除失败：不产生新记录，槽位回到未写入态 ---- */
    CHECK(make_config(&cfg, 6U, 0xB638U));
    CHECK(keystore_write(&cfg));
    CHECK(make_config(&cfg, 4U, 0xDD5EU));
    fake_nvm_fail_next_erase(true);
    CHECK(!keystore_write(&cfg));
    CHECK(!keystore_present());
    CHECK(record_words_are_zero());

    /* ---- 最大长度种子（35 字节）往返；config_crc16 对存储层是不透明值 ---- */
    keystore_config_t max_cfg;
    memset(&max_cfg, 0, sizeof max_cfg);
    max_cfg.algo = KEYSTORE_ALGO_SHA1;
    max_cfg.digits = 6U;
    max_cfg.period_s = 300U;
    max_cfg.secret_len = KEYSTORE_SECRET_MAX_BYTES;
    for (uint8_t i = 0U; i < KEYSTORE_SECRET_MAX_BYTES; i++) {
        max_cfg.secret[i] = (uint8_t)(i + 1U);
    }
    max_cfg.config_crc16 = 0x1234U;
    CHECK(keystore_write(&max_cfg));
    CHECK(keystore_load(&back));
    CHECK(back.secret_len == KEYSTORE_SECRET_MAX_BYTES);
    CHECK(back.period_s == 300U);
    CHECK(memcmp(back.secret, max_cfg.secret, KEYSTORE_SECRET_MAX_BYTES) == 0);
}
