/**
 * @file    test_main.c
 * @brief   主机侧测试入口：Base32 / HMAC-SHA1 / HOTP-TOTP 的 RFC 向量，
 *          以及 lcd_render 的字形与落屏行为。
 */
#include <stdio.h>

unsigned g_checks = 0U;
unsigned g_failures = 0U;

void test_base32(void);
void test_hmac_sha1(void);
void test_lcd_render(void);
void test_totp(void);

int main(void)
{
    test_base32();
    test_hmac_sha1();
    test_lcd_render();
    test_totp();

    printf("共 %u 项断言，失败 %u 项\n", g_checks, g_failures);
    return (g_failures == 0U) ? 0 : 1;
}
