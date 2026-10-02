/**
 * @file    ht1621.c
 * @brief   HT1621B 三线串行驱动实现。
 *
 * 线格式（CS# 上升沿锁存）：
 *   命令  : 100（模式 ID）+ 9 bit 命令码；最低位数据手册标 don't-care，
 *           本驱动恒发 0，故 12 bit 帧可写成 0x800 | 8 bit 码值
 *   写数据: 3 bit 标志 101 + 6 bit 起始地址 + N × 4 bit 数据（地址自动递增）
 *
 * NOTE: 字段位序按数据手册 WRITE 模式时序 `101 A5..A0 D0..D3`：
 *       地址字段 **MSB 先出**，数据半字节 **LSB 先出**（D0 = COM0）。
 *       两者顺序不同，改动本文件的输出顺序前必须实机复验：
 *       数据位序写反会让整屏 COM 行镜像（DOT 变成 C 段、A 段与 D 段互换等）。
 *
 * NOTE: 命令码见下方 CMD_*，与数据手册命令表一一对应。
 *
 * NOTE: 串行时序按数据手册 @3 V：WR 时钟 4–150 kHz，写模式 WR 脉宽
 *       3.34–125 µs，tsu ≥ 60 ns、th ≥ 250 ns、tsu1 ≥ 500 ns、th1 ≥ 250 ns。
 *       本工程内核固定为 MSI 2.097 MHz（见 main.c），所有 WR/CS 边沿之间
 *       都插入显式忙等，不再依赖“指令周期碰巧够慢”。
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

/* ---- 时序常量（仅两个，内核时钟变化时只需重标定此处）----
 * 反汇编复核：ht_delay_loops 每轮 13–15 个周期 ≈ 6–7 µs @ MSI 2.097 MHz
 * （volatile 计数器每轮 2 次 SRAM 访问 + 分支开销）。
 * 2 次 ≈ 12–14 µs，落在 tCLK 允许的 3.34–125 µs 内，建立/保持余量充足。 */
#define HT_WR_DELAY_LOOPS   2U
/* 上电复位等待 tRSTD ≥ 1 ms：400 次 ≈ 2.5 ms。 */
#define HT_POR_DELAY_LOOPS  400U

/**
 * @brief  极粗略忙等，仅用于 HT1621 的串行时序。
 * @param  loops 循环次数，取值为上方 HT_*_DELAY_LOOPS。
 * @note   显式禁止内联/展开，保证时序不随优化档变化（本函数就是时序本身）。
 */
static void __attribute__((noinline)) ht_delay_loops(uint32_t loops)
{
    for (volatile uint32_t i = 0U; i < loops; i++) {
        __asm volatile ("nop");
    }
}

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
 * @brief  按位输出 value 的低 bits 位。
 * @param  lsb_first true = 先出最低位（数据半字节 D0..D3），
 *                   false = 先出最高位（命令码与地址字段 A5..A0）。
 * @note   HT1621 在 WR 上升沿采样 DATA。WR 低/高电平都必须满足 tCLK(min)，
 *         数据在上升沿前建立（tsu）、上升沿后保持（th），故每个边沿间都忙等。
 */
static void write_bits(uint32_t value, uint8_t bits, bool lsb_first)
{
    for (uint8_t i = 0; i < bits; i++) {
        const uint8_t shift = lsb_first ? i : (uint8_t)(bits - 1U - i);
        wr_low();
        ht_delay_loops(HT_WR_DELAY_LOOPS);   /* WR 低电平宽度 */
        data_out((value >> shift) & 1U);
        ht_delay_loops(HT_WR_DELAY_LOOPS);   /* tsu：上升沿前数据已稳定 */
        wr_high();
        ht_delay_loops(HT_WR_DELAY_LOOPS);   /* WR 高电平宽度，兼作 th 数据保持 */
    }
}

static void send_command(uint8_t cmd_code)
{
    cs_low();
    ht_delay_loops(HT_WR_DELAY_LOOPS);       /* tsu1：CS 先于首个 WR 下降沿 */
    write_bits(CMD_MODE_CMD | cmd_code, 12U, false);
    /* 最后一次 wr_high 后 write_bits 内已等待，满足 th1：CS 抬起前的保持 */
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
    ht_delay_loops(HT_WR_DELAY_LOOPS);         /* tsu1：CS 建立 */
    write_bits(0x05U, 3U, false);                     /* 写数据标志 101 */
    write_bits((uint32_t)(addr & 0x3FU), 6U, false);  /* 起始地址 A5..A0，MSB 先出 */
    for (uint8_t i = 0; i < len; i++) {
        write_bits((uint32_t)(data[i] & 0x0FU), 4U, true);  /* 数据 D0..D3，LSB 先出 */
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
    if (on) {
        /* SYS DIS 会同时停振并关闭 LCD 偏压发生器，唤醒须两者都恢复 */
        send_command(CMD_LCD_ON);
        send_command(CMD_SYS_EN);
    } else {
        /* 数据手册推荐的 Power down 顺序：先关偏压发生器，再停振 */
        send_command(CMD_LCD_OFF);
        send_command(CMD_SYS_DIS);
    }
}

void ht1621_init(void)
{
    configure_gpio();
    ht_delay_loops(HT_POR_DELAY_LOOPS);  /* tRSTD ≥ 1 ms：等上电复位完成 */
    send_command(CMD_SYS_DIS);
    send_command(CMD_RC_256K);
    send_command(CMD_BIAS_1_3);
    ht1621_clear();
    send_command(CMD_LCD_ON);
    send_command(CMD_SYS_EN);
}
