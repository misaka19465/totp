/**
 * @file    nvm.h
 * @brief   STM32L031 数据 EEPROM 的字级读写（1 KB @ 0x08080000）。
 *
 * STM32L0 的数据 EEPROM 以 **32 bit 字**为最小擦写单位，没有页擦除：
 *   - 擦除 = 向该字写 0x00000000；
 *   - 写入只允许发生在全 0（已擦除）的字上，向非 0 字写不同值会置
 *     NOTZEROERR 并可能破坏 ECC。
 * 因此本模块要求调用方先擦除后写入；对"非 0 且与目标值不同"的字，
 * nvm_program_words() 在触碰硬件前返回 NVM_ERR_NOT_ERASED。
 *
 * NOTE: 本模块只提供字级原语，不定义任何持久化格式；
 *       记录布局、幂等与读回比对属 app/keystore。
 *       擦除时会跳过已是 0 的字，重复擦除不会产生额外擦写磨损。
 *
 * NOTE: 调用方义务：本模块不判断时钟/电源状态。EEPROM 编程需要内核时钟，
 *       只能在 Run / Low-power run 下调用；Stop 模式中须先唤醒再写。
 */
#ifndef NVM_H
#define NVM_H

#include <stdint.h>

/** 数据 EEPROM 容量与字长（STM32L031F6：1 KB，256 字） */
#define NVM_SIZE_BYTES  1024U
#define NVM_WORD_BYTES  4U
#define NVM_WORD_COUNT  (NVM_SIZE_BYTES / NVM_WORD_BYTES)

/** 操作结果 */
typedef enum {
    NVM_OK = 0,
    NVM_ERR_PARAM,       /**< 索引/长度越界，或 data 为 NULL              */
    NVM_ERR_NOT_ERASED,  /**< 目标字非 0 且与待写值不同，须先擦除         */
    NVM_ERR_TIMEOUT,     /**< BSY 自旋超时                                */
    NVM_ERR_HW,          /**< FLASH_SR 报错（WRPERR/NOTZEROERR/...）      */
    NVM_ERR_VERIFY,      /**< 写入后单字读回不一致                        */
} nvm_status_t;

/**
 * @brief  读取连续若干字。
 * @param  word_index 起始字索引（0..255）。
 * @param  out        输出缓冲，word_count 个字；word_count == 0 时可为 NULL。
 * @param  word_count 字数。
 * @return NVM_OK 或 NVM_ERR_PARAM。
 */
nvm_status_t nvm_read_words(uint16_t word_index, uint32_t *out, uint16_t word_count);

/**
 * @brief  把连续若干字写入 EEPROM（要求目标字已擦除）。
 * @param  word_index 起始字索引（0..255）。
 * @param  data       待写数据；word_count == 0 时可为 NULL。
 * @param  word_count 字数。
 * @return NVM_OK；硬件/校验错误见 nvm_status_t。
 * @note   对已经是目标值的字跳过写入；每个字写后立即读回比对，
 *         任何一字失败即中止（已写入的字保持在 EEPROM 中，由调用方决定回滚）。
 */
nvm_status_t nvm_program_words(uint16_t word_index, const uint32_t *data, uint16_t word_count);

/**
 * @brief  擦除连续若干字（写 0），已为 0 的字跳过。
 * @param  word_index 起始字索引（0..255）。
 * @param  word_count 字数。
 * @return NVM_OK；硬件错误见 nvm_status_t。
 * @note   逐字擦除并读回确认，无整片擦除概念。
 */
nvm_status_t nvm_erase_words(uint16_t word_index, uint16_t word_count);

#endif /* NVM_H */
