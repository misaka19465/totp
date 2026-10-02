/**
 * @file    ht1621.h
 * @brief   HT1621B LCD 驱动（3 线串行接口）与 EDS826 段码屏的位映射。
 *
 * 器件：HT1621BMS/TR（MSKSEMI，SSOP-28；HT1621B 兼容，引脚号以本板原理图为准），
 *       1/3 bias、1/4 duty，片内 RC 振荡器 256 kHz，
 *       显示 RAM 为 32 地址 × 4 bit（每个地址对应一个 SEG 的四路 COM 位）。
 *
 * 显示屏：EDS826S(T)P15V3B（DALIAN QIYUN），6 位 7 段 + 5 个小数点，
 *         TRANSFLECTIVE / NEGATIVE / 1-4DUTY, 1-3BIAS，工作电压 3.0 V。
 *
 * ---- 位映射（依据屏规格书第 2 页引脚表，已与原理图网表逐脚核对）----
 * 原理图把 U2 的 SEG0..SEG5 → 屏 pin 5..10，SEG13..SEG18 → 屏 pin 11..16。
 * 注意第二段是 **SEG13..SEG18**（不是 SEG6..SEG11），因此 4~6 位的 RAM 地址
 * 不连续，落在 0x0D..0x12：
 *
 *   RAM 地址(SEG)  屏引脚  COM0   COM1   COM2   COM3
 *     0x00 (SEG0)    5      1A     1F     1E     1D
 *     0x01 (SEG1)    6      DOT1   1B     1G     1C
 *     0x02 (SEG2)    7      2A     2F     2E     2D
 *     0x03 (SEG3)    8      DOT2   2B     2G     2C
 *     0x04 (SEG4)    9      3A     3F     3E     3D
 *     0x05 (SEG5)   10      ——     3B     3G     3C
 *     0x0D (SEG13)  11      4A     4F     4E     4D
 *     0x0E (SEG14)  12      DOT4   4B     4G     4C
 *     0x0F (SEG15)  13      5A     5F     5E     5D
 *     0x10 (SEG16)  14      DOT5   5B     5G     5C
 *     0x11 (SEG17)  15      6A     6F     6E     6D
 *     0x12 (SEG18)  16      ——     6B     6G     6C
 *
 * 规律：第 n 位占两个地址 —— 第 1~3 位为 0x00/0x01、0x02/0x03、0x04/0x05，
 * 第 4~6 位为 0x0D/0x0E、0x0F/0x10、0x11/0x12；每组偶数地址的 COM0..COM3
 * 依次是 A/F/E/D，奇数地址的 COM0 是小数点（或无）、COM1..COM3 依次是 B/G/C。
 *
 * ---- 由引脚表导出的显示约束 ----
 *   1. **6 位数字全部是完整 7 段**（A 段齐备），0~9 与 A~F 都能正常显示。
 *   2. **小数点只有 4 个**（DOT3 不存在），位置与地址为：
 *        DOT1 = 第 1 位右上（第 1/2 位之间上方）  0x01.COM0
 *        DOT2 = 第 2 位右上（第 2/3 位之间上方）  0x03.COM0
 *        DOT4 = 第 4 位右下（第 4/5 位之间下方）  0x0E.COM0
 *        DOT5 = 第 5 位右下（第 5/6 位之间下方）  0x10.COM0
 *      第 3、6 位没有小数点。
 *
 * 段码屏品牌：DALIAN QIYUN DISPLAY（大连奇耘）EDS826S(T)P15V3B，1/4 duty、1/3 bias。
 */
#ifndef HT1621_H
#define HT1621_H

#include <stdbool.h>
#include <stdint.h>

#define HT1621_SEG_COUNT       12U  /* 本板实际引出 SEG0..SEG5 + SEG13..SEG18 */
#define HT1621_ADDR_MAX        32U  /* 器件完整显示 RAM 地址空间 0x00..0x1F   */

/* ---- 本板显示 RAM 地址（与 docs/hardware.md 映射表一一对应）---- */
/* NOTE: 两组地址不连续：屏 pin 11..16 接的是 SEG13..SEG18，
 *       因此第 4~6 位落在 0x0D..0x12，而不是紧接第 3 位之后的 0x06。 */
#define HT1621_ADDR_DIGIT123  0x00U  /* 第 1~3 位 A/F/E/D 起址，写 6 字节覆盖 0x00..0x05 */
#define HT1621_ADDR_DIGIT456  0x0DU  /* 第 4~6 位 A/F/E/D 起址，写 6 字节覆盖 0x0D..0x12 */

/* 四个状态小数点所在地址（均位于 COM0，即 nibble 的 bit0） */
#define HT1621_DOT_NO_SEED_ADDR   0x01U  /* DOT1 未写入种子   */
#define HT1621_DOT_NEED_SYNC_ADDR 0x03U  /* DOT2 需要重新对时 */
#define HT1621_DOT_LOW_BAT_ADDR   0x0EU  /* DOT4 低电量       */
#define HT1621_DOT_FAULT_ADDR     0x10U  /* DOT5 故障         */
#define HT1621_DOT_BIT             0x01U  /* 各点占 COM0 = nibble bit0 */

/**
 * @brief  初始化 HT1621B 的 GPIO 与显示 RAM，并使能显示。
 * @note   调用前系统时钟须已稳定；本函数不改变 MCU 低功耗状态。
 */
void ht1621_init(void);

/**
 * @brief  向 HT1621B 显示 RAM 写入连续数据。
 * @param  addr 起始地址（0x00..0x1F），每个地址 4 bit，对应 COM0..COM3。
 * @param  data 数据数组，每个元素仅低 4 位有效；bit0=COM0 … bit3=COM3。
 * @param  len  写入的地址个数，调用方保证 addr + len 不超过 32。
 * @note   地址在器件内部自动递增，跨 0x1F 不自动回绕，调用方需自行分段。
 * @note   线序按数据手册 WRITE 时序 `101 A5..A0 D0..D3`：地址字段 MSB 先出、
 *         数据半字节 LSB 先出（D0=COM0）；调用方只按上面的 bit0..bit3 语义传值。
 * @note   2026-10-02 实机曾因数据半字节按 MSB 先出而整屏 COM 行镜像
 *         （状态点显示成 C 段竖条）。改动本函数位序后必须重做 COM 行复验：
 *         向 0x00..0x05、0x0D..0x12 各写 0x1 应点亮 6 条 A 段 + 4 个状态点，
 *         且不出现任何 C 段；映射与实测记录见 docs/hardware.md「段码屏映射」。
 */
void ht1621_write(uint8_t addr, const uint8_t *data, uint8_t len);

/**
 * @brief  清空全部显示 RAM（32 × 4 bit）。
 */
void ht1621_clear(void);

/**
 * @brief  打开/关闭 HT1621B 系统（振荡器与 LCD 偏压发生器）。
 * @param  on true 恢复振荡器与显示；false 进入 Power down，显示变空。
 * @note   on=false 按数据手册顺序先发 LCD OFF 关偏压发生器，再发 SYS DIS
 *         停振进入 Power down：显示变空，芯片静态电流降到 ISTB（3 V 典型
 *         0.1 µA、最大 5 µA），与"只停波形"不同。本设备要求屏幕常亮，
 *         运行期间不得调用 on=false。
 * @note   on=true 会补发 LCD ON 再 SYS EN，显示内容恢复为 RAM 中的旧值
 *         （SYS EN 单独不能重新打开偏压发生器）。
 *         本板 VLCD 直连电池、未做断电器件；整机功耗预算见 docs/hardware.md。
 */
void ht1621_oscillator(bool on);

#endif /* HT1621_H */
