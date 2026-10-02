/**
 * @file    fake_ht1621.h
 * @brief   主机侧测试替身：捕获 lcd_render_flush 对 ht1621_write 的调用。
 *
 * 用途：在不接触寄存器的情况下验证"两组不连续地址、各 6 字节"的落屏行为。
 */
#ifndef FAKE_HT1621_H
#define FAKE_HT1621_H

#include <stdbool.h>
#include <stdint.h>

#define FAKE_HT1621_MAX_WRITES 8U
#define FAKE_HT1621_MAX_LEN    32U

typedef struct {
    uint8_t addr;
    uint8_t len;
    uint8_t data[FAKE_HT1621_MAX_LEN];
} fake_ht1621_write_t;

/** 清空捕获记录 */
void fake_ht1621_reset(void);

/** 返回捕获到的第 index 次写入；越界返回 NULL（index 从 0 计） */
const fake_ht1621_write_t *fake_ht1621_write(unsigned index);

/** 返回捕获到的写入次数 */
unsigned fake_ht1621_write_count(void);

/** 捕获缓冲是否曾溢出（正常测试必须为 false） */
bool fake_ht1621_overflowed(void);

#endif /* FAKE_HT1621_H */
