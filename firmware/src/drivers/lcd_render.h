/**
 * @file    lcd_render.h
 * @brief   段码屏渲染：字形表、按位右对齐写数、状态小数点。
 *
 * 位映射依据 ht1621.h：第 1~3 位占 RAM 地址 0x00..0x05，
 * 第 4~6 位占 0x0D..0x12（两组不连续）；每位的偶数地址半区 COM0..COM3 依次是
 * A/F/E/D，奇数地址半区 COM0 是小数点（第 3、6 位无点）、COM1..COM3 依次是 B/G/C。
 *
 * NOTE: 本模块只负责"数字/状态点 → 显示 RAM 映像"的计算与落屏，不判定状态语义。
 *       "故障时清空数字位（显示 ------）"由调用方以 digits = 0 表达；
 *       四个状态点的语义与触发条件由调用方决定。
 */
#ifndef LCD_RENDER_H
#define LCD_RENDER_H

#include <stdbool.h>
#include <stdint.h>

/** 显示 RAM 映像的单元数：本板实际引出的 12 个 SEG（两组各 6 个地址） */
#define LCD_RENDER_SEG_COUNT  12U

/** 可显示的最大位数（本板 6 位段码） */
#define LCD_RENDER_DIGITS_MAX 6U

/* ---- 状态点掩码（DOT1/DOT2/DOT4/DOT5） ---- */
#define LCD_DOT_NO_SEED   (1U << 0)  /* DOT1：未写入种子       */
#define LCD_DOT_NEED_SYNC (1U << 1)  /* DOT2：需要重新对时     */
#define LCD_DOT_LOW_BAT   (1U << 2)  /* DOT4：低电量           */
#define LCD_DOT_FAULT     (1U << 3)  /* DOT5：故障             */

/**
 * @brief  把口令与状态点渲染成显示 RAM 映像（纯计算，不访问硬件）。
 * @param  code      口令数值；只取十进制最低 digits 位
 * @param  digits    显示位数，0..LCD_RENDER_DIGITS_MAX；0 表示数字位全部留空
 * @param  dot_mask  状态点掩码，LCD_DOT_* 的按位或
 * @param  image     输出映像（长度 LCD_RENDER_SEG_COUNT）：
 *                   image[0..5] → 地址 0x00..0x05，image[6..11] → 0x0D..0x12；
 *                   每单元 bit0=COM0 … bit3=COM3
 * @return 成功返回 true；image == NULL 或 digits > 6 返回 false（不修改映像）
 * @note   位数不足 6 时数字**右对齐、高位留空**（不补零扩展）；
 *         位数以内的前导零会正常显示（如 digits=6、code=42 → "000042"）。
 *         状态点与数字位相互独立，数字位留空时点仍可点亮。
 */
bool lcd_render_image(uint32_t code, uint8_t digits, uint8_t dot_mask, uint8_t *image);

/**
 * @brief  把显示 RAM 映像写入 HT1621B。
 * @param  image 由 lcd_render_image() 得到的映像
 * @note   两组地址不连续，必须分两次连续写（0x00 起 6 字节、0x0D 起 6 字节）。
 */
void lcd_render_flush(const uint8_t *image);

/**
 * @brief  渲染并落屏（lcd_render_image() + lcd_render_flush()）。
 * @return 参数非法（digits > 6）返回 false 且不写屏；否则 true。
 */
bool lcd_render_show(uint32_t code, uint8_t digits, uint8_t dot_mask);

#endif /* LCD_RENDER_H */
