/**
 * @file    lcd_render.c
 * @brief   段码屏渲染实现（字形表 + 右对齐写数 + 状态小数点）。
 */
#include "lcd_render.h"

#include "ht1621.h"

#include <stddef.h>

/* ---- 段位常量 ----
 * 偶数地址半区（COM0..COM3）= A/F/E/D；
 * 奇数地址半区（COM0..COM3）= 小数点/B/G/C（第 3、6 位 COM0 无连接）。 */
#define SEG_A   0x1U
#define SEG_F   0x2U
#define SEG_E   0x4U
#define SEG_D   0x8U
#define SEG_DOT 0x1U
#define SEG_B   0x2U
#define SEG_G   0x4U
#define SEG_C   0x8U

/* 字形表：按十进制数字 0..9 索引。
 * 数字 0 无 G 段，数字 2 无 C 段、数字 4 无 A/D 段、数字 5 无 B/E 段、
 * 数字 7 无 D/E/F/G 段 —— 由两张表显式给出，不做运行时拼装。 */
static const uint8_t GLYPH_AFED[10] = {
    SEG_A | SEG_F | SEG_E | SEG_D,  /* 0 */
    0U,                             /* 1 */
    SEG_A | SEG_E | SEG_D,          /* 2 */
    SEG_A | SEG_D,                  /* 3 */
    SEG_F,                          /* 4 */
    SEG_A | SEG_F | SEG_D,          /* 5 */
    SEG_A | SEG_F | SEG_E | SEG_D,  /* 6 */
    SEG_A,                          /* 7 */
    SEG_A | SEG_F | SEG_E | SEG_D,  /* 8 */
    SEG_A | SEG_F | SEG_D,          /* 9 */
};

static const uint8_t GLYPH_BGC[10] = {
    SEG_B | SEG_C,           /* 0 */
    SEG_B | SEG_C,           /* 1 */
    SEG_B | SEG_G,           /* 2 */
    SEG_B | SEG_G | SEG_C,   /* 3 */
    SEG_B | SEG_G | SEG_C,   /* 4 */
    SEG_G | SEG_C,           /* 5 */
    SEG_G | SEG_C,           /* 6 */
    SEG_B | SEG_C,           /* 7 */
    SEG_B | SEG_G | SEG_C,   /* 8 */
    SEG_B | SEG_G | SEG_C,   /* 9 */
};

/* 位序 0..5（第 1..6 位）对应的状态点；0 = 该位无小数点 */
static const uint8_t DIGIT_DOT_BIT[LCD_RENDER_DIGITS_MAX] = {
    LCD_DOT_NO_SEED, LCD_DOT_NEED_SYNC, 0U, LCD_DOT_LOW_BAT, LCD_DOT_FAULT, 0U,
};

/**
 * @brief  位序（0 = 第 1 位）→ 映像中该位偶数地址半区的下标。
 * @note   第 1~3 位占映像 0..5，第 4~6 位跳到 6..11（对应 RAM 0x0D..0x12）。
 */
static uint8_t slot_of_digit(uint8_t pos)
{
    return (pos < 3U) ? (uint8_t)(2U * pos)
                      : (uint8_t)(6U + 2U * (pos - 3U));
}

bool lcd_render_image(uint32_t code, uint8_t digits, uint8_t dot_mask, uint8_t *image)
{
    if (image == NULL || digits > LCD_RENDER_DIGITS_MAX) {
        return false;
    }

    for (uint8_t i = 0U; i < LCD_RENDER_SEG_COUNT; i++) {
        image[i] = 0U;
    }

    /* 从最低十进制位开始，右对齐落位：i = 0 写最右一位（第 6 位） */
    for (uint8_t i = 0U; i < digits; i++) {
        const uint8_t digit = (uint8_t)(code % 10U);
        code /= 10U;
        const uint8_t slot = slot_of_digit((uint8_t)(LCD_RENDER_DIGITS_MAX - 1U - i));
        image[slot] = GLYPH_AFED[digit];
        image[slot + 1U] = GLYPH_BGC[digit];
    }

    /* 状态点独立于点亮的数字位；数字位留空时点仍点亮 */
    for (uint8_t pos = 0U; pos < LCD_RENDER_DIGITS_MAX; pos++) {
        if ((dot_mask & DIGIT_DOT_BIT[pos]) != 0U) {
            image[slot_of_digit(pos) + 1U] |= SEG_DOT;
        }
    }

    return true;
}

void lcd_render_flush(const uint8_t *image)
{
    if (image == NULL) {
        return;
    }
    ht1621_write(HT1621_ADDR_DIGIT123, image, 6U);
    ht1621_write(HT1621_ADDR_DIGIT456, &image[6], 6U);
}

bool lcd_render_show(uint32_t code, uint8_t digits, uint8_t dot_mask)
{
    uint8_t image[LCD_RENDER_SEG_COUNT];
    if (!lcd_render_image(code, digits, dot_mask, image)) {
        return false;
    }
    lcd_render_flush(image);
    return true;
}
