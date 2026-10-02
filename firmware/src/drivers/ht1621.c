/**
 * @file    ht1621.c
 * @brief   HT1621B 三线串行驱动实现。
 *
 * 线格式（MSB 先出，CS# 上升沿锁存）：
 *   命令  : 12 bit = 1000_0000_CCCC_CCCC，其中低 8 位为命令码
 *   写数据: 3 bit 标志 101 + 6 bit 起始地址 + N × 4 bit 数据（地址自动递增）
 *
 * NOTE: 命令字的高 4 bit 固定为 4'b1000 是 HT1621 的命令模式前缀，
 *       低 8 位命令码与数据手册命令表一一对应，见下方 CMD_* 定义。
 */
#include "ht1621.h"
#include "board_pins.h"
#include "stm32l031xx.h"

/* 命令码（数据手册命令表，8 bit） */
#define CMD_SYS_DIS     0x00U  /* 关闭系统振荡器与 LCD 偏压发生器 */
#define CMD_SYS_EN      0x02U  /* 打开系统振荡器 */
#define CMD_LCD_OFF     0x04U  /* 关闭 LCD 偏压发生器 */
#define CMD_LCD_ON      0x06U  /* 打开 LCD 偏压发生器 */
#define CMD_RC_256K     0x30U  /* 系统时钟源 = 片内 RC 振荡器 256 kHz */
#define CMD_BIAS_1_3    0x52U  /* 1/3 bias，4 COM（本板 COM0..COM3 全用） */
#define CMD_MODE_CMD    0x800U /* 命令模式前缀 1000 */

#define LCD_RAM_NIBBLES 32U    /* HT1621B 显示 RAM：32 地址 × 4 bit */

static void cs_low(void)   { LCD_CS_PORT->BRR  = (1U << LCD_CS_PIN); }
static void cs_high(void)  { LCD_CS_PORT->BSRR = (1U << LCD_CS_PIN); }
static void wr_low(void)   { LCD_WR_PORT->BRR  = (1U << LCD_WR_PIN); }
static void wr_high(void)  { LCD_WR_PORT->BSRR = (1U << LCD_WR_PIN); }

static void data_out(unsigned bit)
{
    if (bit) {
        LCD_DATA_PORT->BSRR = (1U << LCD_DATA_PIN);
    } else {
        LCD_DATA_PORT->BRR  = (1U << LCD_DATA_PIN);
    }
}

/**
 * @brief  MSB 先出地输出 value 的低 bits 位。
 * @note   HT1621 在 WR# 上升沿采样 DATA，此处不额外插入延时：
 *         72 MHz 内核下 GPIO 翻转间隔已远超数据手册的建立/保持下限（约百 ns 量级）。
 */
static void write_bits(uint32_t value, uint8_t bits)
{
    for (uint8_t i = 0; i < bits; i++) {
        wr_low();
        data_out((value >> (bits - 1U - i)) & 1U);
        wr_high();
    }
}

static void send_command(uint8_t cmd_code)
{
    cs_low();
    write_bits(CMD_MODE_CMD | cmd_code, 12U);
    cs_high();
}

static void configure_gpio(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;

    const uint32_t mask =
        (3U << (LCD_CS_PIN   * 2U)) |
        (3U << (LCD_WR_PIN   * 2U)) |
        (3U << (LCD_DATA_PIN * 2U));

    LCD_CS_PORT->MODER = (LCD_CS_PORT->MODER & ~mask) |
        (1U << (LCD_CS_PIN * 2U)) | (1U << (LCD_WR_PIN * 2U)) | (1U << (LCD_DATA_PIN * 2U));

    /* 推挽输出、最低速度档，降低开关噪声与动态功耗 */
    LCD_CS_PORT->OTYPER  &= ~((1U << LCD_CS_PIN) | (1U << LCD_WR_PIN) | (1U << LCD_DATA_PIN));
    LCD_CS_PORT->OSPEEDR &= ~((3U << (LCD_CS_PIN * 2U)) | (3U << (LCD_WR_PIN * 2U)) |
                              (3U << (LCD_DATA_PIN * 2U)));

    cs_high();
    wr_high();
    data_out(0U);
}

void ht1621_write(uint8_t addr, const uint8_t *data, uint8_t len)
{
    cs_low();
    write_bits(0x05U, 3U);                     /* 写数据标志 101 */
    write_bits((uint32_t)(addr & 0x3FU), 6U);  /* 起始地址 A5..A0 */
    for (uint8_t i = 0; i < len; i++) {
        write_bits((uint32_t)(data[i] & 0x0FU), 4U);
    }
    cs_high();
}

void ht1621_clear(void)
{
    uint8_t zeros[LCD_RAM_NIBBLES] = {0};
    ht1621_write(0x00U, zeros, LCD_RAM_NIBBLES);
}

void ht1621_oscillator(bool on)
{
    send_command(on ? CMD_SYS_EN : CMD_SYS_DIS);
}

void ht1621_init(void)
{
    configure_gpio();
    send_command(CMD_SYS_DIS);
    send_command(CMD_RC_256K);
    send_command(CMD_BIAS_1_3);
    ht1621_clear();
    send_command(CMD_LCD_ON);
    send_command(CMD_SYS_EN);
}
