/**
 * @file    test_main.c
 * @brief   主机侧测试入口：Base32 / HMAC-SHA1 / HOTP-TOTP 的 RFC 向量，
 *          lcd_render 的字形与落屏行为、uart_link 的帧与链路状态机，
 *          keystore 的单槽持久化语义（CRC、幂等、失败回滚），
 *          以及 rtc_time 的日历/亚秒/校准折算。
 */
#include <stdio.h>

unsigned g_checks = 0U;
unsigned g_failures = 0U;

void test_base32(void);
void test_hmac_sha1(void);
void test_lcd_render(void);
void test_keystore(void);
void test_rtc_time(void);
void test_totp(void);
void test_uart_link(void);

int main(void)
{
    test_base32();
    test_hmac_sha1();
    test_keystore();
    test_lcd_render();
    test_rtc_time();
    test_totp();
    test_uart_link();

    printf("共 %u 项断言，失败 %u 项\n", g_checks, g_failures);
    return (g_failures == 0U) ? 0 : 1;
}
