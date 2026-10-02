/**
 * @file    nvm.c
 * @brief   数据 EEPROM 字级读写实现（见 nvm.h）。
 */
#include "nvm.h"

#include <stdbool.h>
#include <stddef.h>

#include "stm32l031xx.h"

/* NOTE: EEPROM 单字擦/写耗时毫秒级（典型 3.3 ms）；本循环按 MSI 2.097 MHz
 *       的最坏情况取值，约 ≥40 ms 才判超时，是单次操作上限的 10 倍以上。
 *       一旦超时立即返回，不会把看门狗（4 s）拖到复位。 */
#define NVM_BUSY_SPIN 200000U

/* NOTE: EOP 与各错误位均为 rc_w1（写 1 清除），BSY/HVOFF/READY 只读。 */
#define FLASH_SR_ERR_MASK (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
                           FLASH_SR_OPTVERR | FLASH_SR_RDERR | FLASH_SR_NOTZEROERR | \
                           FLASH_SR_FWWERR)

static volatile uint32_t *eeprom_word(uint16_t index)
{
    return (volatile uint32_t *)(uintptr_t)(DATA_EEPROM_BASE + ((uint32_t)index * NVM_WORD_BYTES));
}

static bool range_ok(uint16_t word_index, uint16_t word_count)
{
    return ((uint32_t)word_index + (uint32_t)word_count) <= NVM_WORD_COUNT;
}

/* NOTE: 与 HAL 的解锁序列一致：清 PELOCK，中断在两次写键之间必须关闭。 */
static void flash_unlock(void)
{
    if ((FLASH->PECR & FLASH_PECR_PELOCK) != 0U) {
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        FLASH->PEKEYR = 0x89ABCDEFU;
        FLASH->PEKEYR = 0x02030405U;
        __set_PRIMASK(primask);
    }
}

static void flash_lock(void)
{
    FLASH->PECR |= FLASH_PECR_PELOCK;
}

/**
 * @brief  等待 BSY 清零。
 * @return 在自旋上限内结束返回 true；否则 false。
 */
static bool flash_wait_busy(void)
{
    for (uint32_t spin = 0U; spin < NVM_BUSY_SPIN; spin++) {
        if ((FLASH->SR & FLASH_SR_BSY) == 0U) {
            return true;
        }
    }
    return false;
}

/**
 * @brief  收口一次已完成的字操作：检查错误位并写 1 清除 EOP/错误标志。
 * @return 本次操作无错误返回 NVM_OK；有错误位返回 NVM_ERR_HW。
 * @note    POR 后可能残留 OPTVERR 等历史标志，因此只在操作完成后调用；
 *          解锁后的首次写入前不判错，避免把上电残留误报为本模块故障。
 */
static nvm_status_t flash_take_result(void)
{
    const uint32_t sr = FLASH->SR;
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_ERR_MASK;
    return ((sr & FLASH_SR_ERR_MASK) == 0U) ? NVM_OK : NVM_ERR_HW;
}

nvm_status_t nvm_read_words(uint16_t word_index, uint32_t *out, uint16_t word_count)
{
    if (!range_ok(word_index, word_count) || (out == NULL && word_count > 0U)) {
        return NVM_ERR_PARAM;
    }
    for (uint16_t i = 0U; i < word_count; i++) {
        out[i] = *eeprom_word((uint16_t)(word_index + i));
    }
    return NVM_OK;
}

nvm_status_t nvm_program_words(uint16_t word_index, const uint32_t *data, uint16_t word_count)
{
    if (!range_ok(word_index, word_count) || (data == NULL && word_count > 0U)) {
        return NVM_ERR_PARAM;
    }

    /* 先做擦除前提检查：绝不对非 0 字执行硬件写入 */
    for (uint16_t i = 0U; i < word_count; i++) {
        const uint32_t current = *eeprom_word((uint16_t)(word_index + i));
        if (current != 0U && current != data[i]) {
            return NVM_ERR_NOT_ERASED;
        }
    }

    nvm_status_t status = NVM_OK;
    flash_unlock();
    if (!flash_wait_busy()) {
        flash_lock();
        return NVM_ERR_TIMEOUT;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        const uint16_t index = (uint16_t)(word_index + i);
        if (*eeprom_word(index) == data[i]) {
            continue;                       /* 已是目标值，零磨损跳过 */
        }
        *eeprom_word(index) = data[i];
        if (!flash_wait_busy()) {
            status = NVM_ERR_TIMEOUT;
            break;
        }
        status = flash_take_result();
        if (status != NVM_OK) {
            break;
        }
        if (*eeprom_word(index) != data[i]) {
            status = NVM_ERR_VERIFY;
            break;
        }
    }

    flash_lock();
    return status;
}

nvm_status_t nvm_erase_words(uint16_t word_index, uint16_t word_count)
{
    if (!range_ok(word_index, word_count)) {
        return NVM_ERR_PARAM;
    }

    nvm_status_t status = NVM_OK;
    flash_unlock();
    if (!flash_wait_busy()) {
        flash_lock();
        return NVM_ERR_TIMEOUT;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        const uint16_t index = (uint16_t)(word_index + i);
        if (*eeprom_word(index) == 0U) {
            continue;                       /* 已是未写入态，零磨损跳过 */
        }
        *eeprom_word(index) = 0U;
        if (!flash_wait_busy()) {
            status = NVM_ERR_TIMEOUT;
            break;
        }
        status = flash_take_result();
        if (status != NVM_OK) {
            break;
        }
        if (*eeprom_word(index) != 0U) {
            status = NVM_ERR_VERIFY;
            break;
        }
    }

    flash_lock();
    return status;
}
