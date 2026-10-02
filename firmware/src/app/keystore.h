/**
 * @file    keystore.h
 * @brief   单槽点位配置的持久化存储（数据 EEPROM，断电不丢）。
 *
 * 记录内容 = TOTP 参数（algo/digits/period）+ 解码后的种子 + config_crc16。
 * 存储格式与协议无关，字段约束来自 docs/serial-protocol.md §5.6：
 *   algo = 1（HMAC-SHA1）、digits 1..6、period 15..300 s、
 *   Base32 种子 1..56 字符（解码后 ≤35 字节）。
 *
 * 记录完整性由独立于 config_crc16 的 CRC 保护（同一 CRC-16/CCITT-FALSE）：
 *   config_crc16 是**上位机可见的配置指纹**，覆盖 ASCII 种子，由 proto 层
 *   计算后传入并存回（READ_META 原样返回，见 serial-protocol.md §5.7）；
 *   keystore 无法、也不应从解码后的字节复算它。
 *
 * 失效语义：magic/版本/记录 CRC/字段范围任一不符都视为"未写入种子"，
 * 与全空 EEPROM 同样处理（DOT1 亮）；写入中途掉电只会留下这种可检测的
 * 残记录，不会产生半份被采信的配置。
 */
#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "nvm.h"

/** 记录占用的 EEPROM 字数（固定 52 B 记录） */
#define KEYSTORE_RECORD_WORDS  13U
/** 记录占用的字节数，必须能被 NVM_WORD_BYTES 整除 */
#define KEYSTORE_RECORD_BYTES  (KEYSTORE_RECORD_WORDS * NVM_WORD_BYTES)

/** 支持的算法（serial-protocol.md §5.6：目前仅 HMAC-SHA1） */
#define KEYSTORE_ALGO_SHA1        1U
/** 口令位数范围（本设备 6 位段码屏） */
#define KEYSTORE_DIGITS_MIN       1U
#define KEYSTORE_DIGITS_MAX       6U
/** TOTP 步长范围（秒） */
#define KEYSTORE_PERIOD_MIN_S     15U
#define KEYSTORE_PERIOD_MAX_S     300U
/** 解码后的种子字节数上限（56 Base32 字符 → 35 字节） */
#define KEYSTORE_SECRET_MAX_BYTES 35U

/** 一份点位配置（与 EEPROM 记录的字段一一对应） */
typedef struct {
    uint8_t  algo;                              /**< 仅 KEYSTORE_ALGO_SHA1            */
    uint8_t  digits;                            /**< 1..6                             */
    uint16_t period_s;                          /**< 15..300                          */
    uint8_t  secret_len;                        /**< 1..35                            */
    uint8_t  secret[KEYSTORE_SECRET_MAX_BYTES]; /**< 解码后的种子字节                 */
    uint16_t config_crc16;                      /**< 协议指纹，由调用方计算（可任意值） */
} keystore_config_t;

/**
 * @brief  读取并校验记录。
 * @param  cfg_out 输出配置；可为 NULL，仅查询是否存在有效记录。
 * @return 有效记录返回 true；未写入/记录损坏返回 false（不修改 *cfg_out）。
 * @note   EEPROM 读失败与"无记录"同样返回 false：调用方都按"未写入种子"处理。
 */
bool keystore_load(keystore_config_t *cfg_out);

/**
 * @brief  是否存在有效记录（keystore_load(NULL) 的具名形式）。
 */
bool keystore_present(void);

/**
 * @brief  写入一份配置：先校验参数，再擦除 + 写入 + 逐字节读回比对。
 * @param  cfg 待写入配置；字段越界返回 false 且不改动 EEPROM。
 * @return 落盘并读回一致返回 true；否则记录被擦除（不留半份配置）并返回 false。
 * @note   与现有记录完全相同时跳过物理写入直接返回 true，重传不产生磨损。
 *         config_crc16 由调用方（proto）按 serial-protocol.md §5.7 计算。
 *         最坏耗时 = 13 字擦除 + 13 字写入 ≈ 26 × 3.3 ms < 100 ms，
 *         在协议 §7 的存储命令 200 ms 响应期限内。
 */
bool keystore_write(const keystore_config_t *cfg);

/**
 * @brief  擦除记录（已空时零磨损跳过）。
 * @return 擦除并读回确认成功返回 true。
 */
bool keystore_erase(void);

#endif /* KEYSTORE_H */
