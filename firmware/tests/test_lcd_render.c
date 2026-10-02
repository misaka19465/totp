/**
 * @file    test_lcd_render.c
 * @brief   lcd_render 单元测试：字形、右对齐、状态点与双段地址落屏。
 *
 * 期望值独立于实现的手写字形表给出（不引用 lcd_render.c 的内部数据），
 * 以便在段码映射被改错时立刻失败。
 */
#include "test_util.h"

#include "fake_ht1621.h"
#include "lcd_render.h"

/** 整幅 12 单元映像比对 */
#define CHECK_IMAGE(actual, expected) do {                                  \
    g_checks++;                                                             \
    if (memcmp((actual), (expected), LCD_RENDER_SEG_COUNT) != 0) {          \
        g_failures++;                                                       \
        printf("FAIL %s:%d: image = ", __FILE__, __LINE__);                 \
        test_print_hex((const uint8_t *)(actual), LCD_RENDER_SEG_COUNT);    \
        printf("，期望 ");                                                   \
        test_print_hex((const uint8_t *)(expected), LCD_RENDER_SEG_COUNT);  \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

void test_lcd_render(void)
{
    uint8_t image[LCD_RENDER_SEG_COUNT];

    /* 满 6 位 287082：同时校验字形与映像顺序（第 4~6 位在映像 6..11） */
    static const uint8_t expect_full[LCD_RENDER_SEG_COUNT] = {
        0x0D, 0x06,  /* 2 */
        0x0F, 0x0E,  /* 8 */
        0x01, 0x0A,  /* 7 */
        0x0F, 0x0A,  /* 0 */
        0x0F, 0x0E,  /* 8 */
        0x0D, 0x06,  /* 2 */
    };
    CHECK(lcd_render_image(287082U, 6U, 0U, image));
    CHECK_IMAGE(image, expect_full);

    /* 4 位右对齐：7082 → 第 1、2 位留空，第 3~6 位显示 7/0/8/2 */
    static const uint8_t expect_right[LCD_RENDER_SEG_COUNT] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x0A,
        0x0F, 0x0A, 0x0F, 0x0E, 0x0D, 0x06,
    };
    CHECK(lcd_render_image(7082U, 4U, 0U, image));
    CHECK_IMAGE(image, expect_right);

    /* 位数以内前导零正常显示：42 @ 6 位 → "000042" */
    static const uint8_t expect_pad[LCD_RENDER_SEG_COUNT] = {
        0x0F, 0x0A, 0x0F, 0x0A, 0x0F, 0x0A,
        0x0F, 0x0A, 0x02, 0x0E, 0x0D, 0x06,
    };
    CHECK(lcd_render_image(42U, 6U, 0U, image));
    CHECK_IMAGE(image, expect_pad);

    /* 全部 10 个字形：digits=1 时只取最低十进制位 */
    static const uint8_t glyph_afed[10] = {
        0x0F, 0x00, 0x0D, 0x09, 0x02, 0x0B, 0x0F, 0x01, 0x0F, 0x0B,
    };
    static const uint8_t glyph_bgc[10] = {
        0x0A, 0x0A, 0x06, 0x0E, 0x0E, 0x0C, 0x0C, 0x0A, 0x0E, 0x0E,
    };
    for (uint8_t digit = 0U; digit < 10U; digit++) {
        CHECK(lcd_render_image((uint32_t)digit, 1U, 0U, image));
        for (uint8_t i = 0U; i < 10U; i++) {
            CHECK_U32(image[i], 0x00U);   /* 高位 5 位留空 */
        }
        CHECK_U32(image[10], glyph_afed[digit]);
        CHECK_U32(image[11], glyph_bgc[digit]);
    }

    /* digits=0：数字位全部留空（故障/未配置），code 被忽略 */
    static const uint8_t expect_blank[LCD_RENDER_SEG_COUNT] = {0};
    CHECK(lcd_render_image(12345U, 0U, 0U, image));
    CHECK_IMAGE(image, expect_blank);

    /* 数字位留空时四个状态点仍要点亮（含第 4~6 位的地址跳变） */
    static const uint8_t expect_dots[LCD_RENDER_SEG_COUNT] = {
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    };
    CHECK(lcd_render_image(0U, 0U,
                           LCD_DOT_NO_SEED | LCD_DOT_NEED_SYNC |
                           LCD_DOT_LOW_BAT | LCD_DOT_FAULT, image));
    CHECK_IMAGE(image, expect_dots);

    /* 第 3、6 位无小数点：四点全亮时这两位奇数半区的 COM0 必须保持 0 */
    static const uint8_t expect_no_dot_3_6[LCD_RENDER_SEG_COUNT] = {
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E,
        0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0E,
    };
    CHECK(lcd_render_image(888888U, 6U,
                           LCD_DOT_NO_SEED | LCD_DOT_NEED_SYNC |
                           LCD_DOT_LOW_BAT | LCD_DOT_FAULT, image));
    CHECK_IMAGE(image, expect_no_dot_3_6);

    /* 参数校验：非法输入不修改映像 */
    uint8_t untouched[LCD_RENDER_SEG_COUNT];
    memset(untouched, 0xA5, sizeof untouched);
    CHECK(!lcd_render_image(0U, 7U, 0U, untouched));
    CHECK_U32(untouched[0], 0xA5U);
    CHECK(!lcd_render_image(0U, 0U, 0U, NULL));
    CHECK(!lcd_render_show(0U, 7U, 0U));

    /* 落屏：两组不连续地址各 6 字节，内容与映像严格一致 */
    fake_ht1621_reset();
    CHECK(lcd_render_show(287082U, 6U, LCD_DOT_FAULT));
    CHECK_U32(fake_ht1621_write_count(), 2U);

    const fake_ht1621_write_t *first = fake_ht1621_write(0U);
    const fake_ht1621_write_t *second = fake_ht1621_write(1U);
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        uint8_t expected[LCD_RENDER_SEG_COUNT];
        CHECK(lcd_render_image(287082U, 6U, LCD_DOT_FAULT, expected));
        CHECK_U32(first->addr, 0x00U);
        CHECK_U32(first->len, 6U);
        CHECK(memcmp(first->data, expected, 6U) == 0);
        CHECK_U32(second->addr, 0x0DU);
        CHECK_U32(second->len, 6U);
        CHECK(memcmp(second->data, &expected[6], 6U) == 0);
    }
    CHECK(!fake_ht1621_overflowed());
    CHECK(fake_ht1621_write(2U) == NULL);

    fake_ht1621_reset();
    CHECK(!lcd_render_show(0U, 7U, 0U));
    CHECK_U32(fake_ht1621_write_count(), 0U);
}
