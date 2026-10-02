/**
 * @file    crc16.h
 * @brief   CRC-16/CCITT-FALSE 公共实现（poly=0x1021, init=0xFFFF,
 *          refin/refout=false, xorout=0）。
 *
 * 同一算法服务三处：
 *   - 串口帧校验（drivers/uart_link）；
 *   - config_crc16（app/proto 计算后存入 keystore）；
 *   - keystore 存储记录完整性（app/keystore）。
 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

/** CRC-16/CCITT-FALSE 初值（空输入的结果即此值） */
#define CRC16_CCITT_FALSE_INIT  0xFFFFU

/**
 * @brief  用单个字节推进 CRC。
 * @param  crc  当前 CRC 值（首字节前传 CRC16_CCITT_FALSE_INIT）。
 * @param  byte 输入字节。
 * @return 更新后的 CRC。
 */
uint16_t crc16_update(uint16_t crc, uint8_t byte);

/**
 * @brief  计算一段字节的 CRC-16/CCITT-FALSE。
 * @param  data 待校验字节；len == 0 时可为 NULL。
 * @param  len  字节数。
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

#endif /* CRC16_H */
