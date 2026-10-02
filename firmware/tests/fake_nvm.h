/**
 * @file    fake_nvm.h
 * @brief   主机侧测试替身：用 256 字数组模拟数据 EEPROM。
 *
 * 语义与真实 EEPROM 对齐：写入前目标字必须为 0；擦除 = 写 0；
 * 已为 0 的字擦除时跳过。另提供故障注入（写入/擦除失败）供
 * keystore 的回滚路径测试使用。
 */
#ifndef FAKE_NVM_H
#define FAKE_NVM_H

#include <stdbool.h>
#include <stdint.h>

#include "nvm.h"

/** 清空全部字并复位故障注入与调用计数 */
void fake_nvm_reset(void);

/** 直接访问底层字数组（测试用于篡改记录、模拟位翻转） */
uint32_t *fake_nvm_words(void);

/** 让下一次 nvm_program_words 返回 NVM_ERR_HW（模拟掉电/硬件故障） */
void fake_nvm_fail_next_program(bool fail);

/** 让下一次 nvm_erase_words 返回 NVM_ERR_HW */
void fake_nvm_fail_next_erase(bool fail);

/** 累计的物理写入/擦除调用次数（判断幂等写入是否真的跳过硬件） */
unsigned fake_nvm_program_calls(void);
unsigned fake_nvm_erase_calls(void);

#endif /* FAKE_NVM_H */
