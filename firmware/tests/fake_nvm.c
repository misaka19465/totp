/**
 * @file    fake_nvm.c
 * @brief   nvm 的主机侧测试替身（见 fake_nvm.h）。
 */
#include "fake_nvm.h"

#include <string.h>

static uint32_t s_words[NVM_WORD_COUNT];
static bool s_fail_program = false;
static bool s_fail_erase = false;
static unsigned s_program_calls = 0U;
static unsigned s_erase_calls = 0U;

void fake_nvm_reset(void)
{
    memset(s_words, 0, sizeof s_words);
    s_fail_program = false;
    s_fail_erase = false;
    s_program_calls = 0U;
    s_erase_calls = 0U;
}

uint32_t *fake_nvm_words(void)
{
    return s_words;
}

void fake_nvm_fail_next_program(bool fail)
{
    s_fail_program = fail;
}

void fake_nvm_fail_next_erase(bool fail)
{
    s_fail_erase = fail;
}

unsigned fake_nvm_program_calls(void)
{
    return s_program_calls;
}

unsigned fake_nvm_erase_calls(void)
{
    return s_erase_calls;
}

/* ---- 与 nvm.h 原型一致的测试实现 ---- */

static bool range_ok(uint16_t word_index, uint16_t word_count)
{
    return ((uint32_t)word_index + (uint32_t)word_count) <= NVM_WORD_COUNT;
}

nvm_status_t nvm_read_words(uint16_t word_index, uint32_t *out, uint16_t word_count)
{
    if (!range_ok(word_index, word_count) || (out == NULL && word_count > 0U)) {
        return NVM_ERR_PARAM;
    }
    for (uint16_t i = 0U; i < word_count; i++) {
        out[i] = s_words[word_index + i];
    }
    return NVM_OK;
}

nvm_status_t nvm_program_words(uint16_t word_index, const uint32_t *data, uint16_t word_count)
{
    if (!range_ok(word_index, word_count) || (data == NULL && word_count > 0U)) {
        return NVM_ERR_PARAM;
    }

    s_program_calls++;
    if (s_fail_program) {
        s_fail_program = false;
        return NVM_ERR_HW;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        const uint32_t current = s_words[word_index + i];
        if (current != 0U && current != data[i]) {
            return NVM_ERR_NOT_ERASED;
        }
    }
    for (uint16_t i = 0U; i < word_count; i++) {
        s_words[word_index + i] = data[i];
    }
    return NVM_OK;
}

nvm_status_t nvm_erase_words(uint16_t word_index, uint16_t word_count)
{
    if (!range_ok(word_index, word_count)) {
        return NVM_ERR_PARAM;
    }

    s_erase_calls++;
    if (s_fail_erase) {
        s_fail_erase = false;
        return NVM_ERR_HW;
    }

    for (uint16_t i = 0U; i < word_count; i++) {
        s_words[word_index + i] = 0U;
    }
    return NVM_OK;
}
