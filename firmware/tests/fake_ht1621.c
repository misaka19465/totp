/**
 * @file    fake_ht1621.c
 * @brief   ht1621_write 的主机侧测试替身（见 fake_ht1621.h）。
 */
#include "fake_ht1621.h"

#include "ht1621.h"

#include <string.h>

static fake_ht1621_write_t s_writes[FAKE_HT1621_MAX_WRITES];
static unsigned s_count = 0U;
static bool s_overflowed = false;

void fake_ht1621_reset(void)
{
    s_count = 0U;
    s_overflowed = false;
    memset(s_writes, 0, sizeof s_writes);
}

/** 与 ht1621.h 中的原型一致，取代真实实现参与测试链接 */
void ht1621_write(uint8_t addr, const uint8_t *data, uint8_t len)
{
    if (s_count >= FAKE_HT1621_MAX_WRITES) {
        s_overflowed = true;
        return;
    }

    fake_ht1621_write_t *write = &s_writes[s_count];
    write->addr = addr;
    write->len = (len > FAKE_HT1621_MAX_LEN) ? FAKE_HT1621_MAX_LEN : len;
    if (data != NULL && write->len > 0U) {
        memcpy(write->data, data, write->len);
    }
    s_count++;
}

const fake_ht1621_write_t *fake_ht1621_write(unsigned index)
{
    return (index < s_count) ? &s_writes[index] : NULL;
}

unsigned fake_ht1621_write_count(void)
{
    return s_count;
}

bool fake_ht1621_overflowed(void)
{
    return s_overflowed;
}
