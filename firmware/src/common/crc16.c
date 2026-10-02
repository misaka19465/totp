/**
 * @file    crc16.c
 * @brief   CRC-16/CCITT-FALSE 公共实现（见 crc16.h）。
 */
#include "crc16.h"

/* NOTE: 16 项半字节表，占 32 B Flash；算法逐位实现 CCITT-FALSE，
 *       同一份代码供帧/配置/存储记录复用。 */
static const uint16_t CRC16_NIBBLE[16] = {
    0x0000U, 0x1021U, 0x2042U, 0x3063U, 0x4084U, 0x50A5U, 0x60C6U, 0x70E7U,
    0x8108U, 0x9129U, 0xA14AU, 0xB16BU, 0xC18CU, 0xD1ADU, 0xE1CEU, 0xF1EFU,
};

uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    crc = (uint16_t)((crc << 4) ^ CRC16_NIBBLE[crc >> 12]);
    crc = (uint16_t)((crc << 4) ^ CRC16_NIBBLE[crc >> 12]);
    return crc;
}

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = CRC16_CCITT_FALSE_INIT;
    for (size_t i = 0U; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}
