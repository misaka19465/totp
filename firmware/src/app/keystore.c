/**
 * @file    keystore.c
 * @brief   单槽配置的 EEPROM 记录实现（见 keystore.h）。
 */
#include "keystore.h"

#include <string.h>

#include "crc16.h"

/* ---- 记录字节布局（52 B = 13 字，全部小端） ----
 *   0  u16 magic = 0x4B53（'S''K'，非 0 即"可能已写入"）
 *   2  u8  version = 2
 *   3  u8  flags = bit0 SEED_PRESENT，其余为 0
 *   4  u8  algo
 *   5  u8  digits
 *   6  u16 period_s
 *   8  u8  secret_len（解码后的字节数）
 *   9  u8  secret_chars（Base32 字符数，READ_META 回显）
 *  10  u16 config_crc16
 *  12  35B secret（不足补 0）
 *  47  u8  reserved1 = 0
 *  48  u16 record_crc16（覆盖偏移 0..47）
 *  50  u16 reserved2 = 0
 *
 * NOTE: v1（无 secret_chars，偏移 9 为 0）从未发布，直接升版为 v2 并拒绝 v1
 *       记录（按未写入种子处理）；不提供 v1 迁移路径。
 */
enum {
    KS_OFF_MAGIC = 0,
    KS_OFF_VERSION = 2,
    KS_OFF_FLAGS = 3,
    KS_OFF_ALGO = 4,
    KS_OFF_DIGITS = 5,
    KS_OFF_PERIOD = 6,
    KS_OFF_SECRET_LEN = 8,
    KS_OFF_SECRET_CHARS = 9,
    KS_OFF_CONFIG_CRC = 10,
    KS_OFF_SECRET = 12,
    KS_OFF_RECORD_CRC = 48,
    KS_CRC_COVER_BYTES = KS_OFF_RECORD_CRC,
};

#define KEYSTORE_MAGIC      0x4B53U
#define KEYSTORE_VERSION    2U
#define KS_FLAG_SEED_PRESENT 0x01U

_Static_assert(KEYSTORE_RECORD_BYTES == 52U, "记录布局与 KEYSTORE_RECORD_WORDS 必须一致");

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static void words_to_bytes(const uint32_t *words, uint8_t *bytes)
{
    for (uint8_t i = 0U; i < KEYSTORE_RECORD_WORDS; i++) {
        for (uint8_t b = 0U; b < NVM_WORD_BYTES; b++) {
            bytes[i * NVM_WORD_BYTES + b] = (uint8_t)(words[i] >> (8U * b));
        }
    }
}

static void bytes_to_words(const uint8_t *bytes, uint32_t *words)
{
    for (uint8_t i = 0U; i < KEYSTORE_RECORD_WORDS; i++) {
        uint32_t word = 0U;
        for (uint8_t b = 0U; b < NVM_WORD_BYTES; b++) {
            word |= (uint32_t)bytes[i * NVM_WORD_BYTES + b] << (8U * b);
        }
        words[i] = word;
    }
}

static bool config_in_range(const keystore_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->algo != KEYSTORE_ALGO_SHA1) {
        return false;
    }
    if (cfg->digits < KEYSTORE_DIGITS_MIN || cfg->digits > KEYSTORE_DIGITS_MAX) {
        return false;
    }
    if (cfg->period_s < KEYSTORE_PERIOD_MIN_S || cfg->period_s > KEYSTORE_PERIOD_MAX_S) {
        return false;
    }
    if (cfg->secret_len < 1U || cfg->secret_len > KEYSTORE_SECRET_MAX_BYTES) {
        return false;
    }
    if (cfg->secret_chars < KEYSTORE_SECRET_CHARS_MIN ||
        cfg->secret_chars > KEYSTORE_SECRET_CHARS_MAX) {
        return false;
    }
    /* 字符数与解码长度必须自洽（base32 解码长度 = 字符数 × 5 / 8） */
    if (((size_t)cfg->secret_chars * 5U) / 8U != (size_t)cfg->secret_len) {
        return false;
    }
    return true;
}

static bool config_equal(const keystore_config_t *a, const keystore_config_t *b)
{
    return a->algo == b->algo && a->digits == b->digits &&
           a->period_s == b->period_s && a->secret_chars == b->secret_chars &&
           a->secret_len == b->secret_len && a->config_crc16 == b->config_crc16 &&
           memcmp(a->secret, b->secret, a->secret_len) == 0;
}

static void record_encode(const keystore_config_t *cfg, uint8_t *record)
{
    memset(record, 0, KEYSTORE_RECORD_BYTES);
    put_u16(&record[KS_OFF_MAGIC], KEYSTORE_MAGIC);
    record[KS_OFF_VERSION] = KEYSTORE_VERSION;
    record[KS_OFF_FLAGS] = KS_FLAG_SEED_PRESENT;
    record[KS_OFF_ALGO] = cfg->algo;
    record[KS_OFF_DIGITS] = cfg->digits;
    put_u16(&record[KS_OFF_PERIOD], cfg->period_s);
    record[KS_OFF_SECRET_LEN] = cfg->secret_len;
    record[KS_OFF_SECRET_CHARS] = cfg->secret_chars;
    put_u16(&record[KS_OFF_CONFIG_CRC], cfg->config_crc16);
    memcpy(&record[KS_OFF_SECRET], cfg->secret, cfg->secret_len);
    put_u16(&record[KS_OFF_RECORD_CRC], crc16_ccitt_false(record, KS_CRC_COVER_BYTES));
}

static bool record_parse(const uint8_t *record, keystore_config_t *cfg_out)
{
    if (get_u16(&record[KS_OFF_MAGIC]) != KEYSTORE_MAGIC ||
        record[KS_OFF_VERSION] != KEYSTORE_VERSION ||
        record[KS_OFF_FLAGS] != KS_FLAG_SEED_PRESENT ||
        get_u16(&record[KS_OFF_RECORD_CRC]) != crc16_ccitt_false(record, KS_CRC_COVER_BYTES)) {
        return false;
    }

    /* 防御性字段校验：CRC 正确但字段越界只可能来自不兼容的旧记录/被篡改的镜像，
     * 同样按"未写入种子"处理，避免把不可用配置交给 TOTP。 */
    keystore_config_t cfg;
    cfg.algo = record[KS_OFF_ALGO];
    cfg.digits = record[KS_OFF_DIGITS];
    cfg.period_s = get_u16(&record[KS_OFF_PERIOD]);
    cfg.secret_len = record[KS_OFF_SECRET_LEN];
    cfg.secret_chars = record[KS_OFF_SECRET_CHARS];
    cfg.config_crc16 = get_u16(&record[KS_OFF_CONFIG_CRC]);
    memset(cfg.secret, 0, sizeof cfg.secret);
    memcpy(cfg.secret, &record[KS_OFF_SECRET], KEYSTORE_SECRET_MAX_BYTES);
    if (!config_in_range(&cfg)) {
        return false;
    }

    if (cfg_out != NULL) {
        *cfg_out = cfg;
    }
    return true;
}

static bool record_read(uint8_t *record)
{
    uint32_t words[KEYSTORE_RECORD_WORDS];
    if (nvm_read_words(0U, words, KEYSTORE_RECORD_WORDS) != NVM_OK) {
        return false;
    }
    words_to_bytes(words, record);
    return true;
}

static bool record_matches(const uint8_t *record)
{
    uint32_t words[KEYSTORE_RECORD_WORDS];
    uint8_t readback[KEYSTORE_RECORD_BYTES];
    if (nvm_read_words(0U, words, KEYSTORE_RECORD_WORDS) != NVM_OK) {
        return false;
    }
    words_to_bytes(words, readback);
    return memcmp(record, readback, KEYSTORE_RECORD_BYTES) == 0;
}

bool keystore_load(keystore_config_t *cfg_out)
{
    uint8_t record[KEYSTORE_RECORD_BYTES];
    if (!record_read(record)) {
        return false;
    }
    return record_parse(record, cfg_out);
}

bool keystore_present(void)
{
    return keystore_load(NULL);
}

bool keystore_write(const keystore_config_t *cfg)
{
    if (!config_in_range(cfg)) {
        return false;
    }

    keystore_config_t current;
    if (keystore_load(&current) && config_equal(&current, cfg)) {
        return true;                        /* 幂等：相同配置不产生擦写磨损 */
    }

    uint8_t record[KEYSTORE_RECORD_BYTES];
    uint32_t words[KEYSTORE_RECORD_WORDS];
    record_encode(cfg, record);
    bytes_to_words(record, words);

    const bool ok =
        (nvm_erase_words(0U, KEYSTORE_RECORD_WORDS) == NVM_OK) &&
        (nvm_program_words(0U, words, KEYSTORE_RECORD_WORDS) == NVM_OK) &&
        record_matches(record);

    if (!ok) {
        /* 掉电/硬件故障可能留下半份记录；主动擦除保证失败后是"未写入种子"，
         * 而不是一份可能被采信的坏配置。 */
        (void)nvm_erase_words(0U, KEYSTORE_RECORD_WORDS);
    }
    return ok;
}

bool keystore_erase(void)
{
    return nvm_erase_words(0U, KEYSTORE_RECORD_WORDS) == NVM_OK;
}
