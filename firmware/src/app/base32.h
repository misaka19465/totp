/**
 * @file    base32.h
 * @brief   Base32 种子解码（RFC 4648，无填充）。
 *
 * 规则与 docs/serial-protocol.md §5.6 的 WRITE_SLOT.secret 一致：
 *   仅接受 A-Z 与 2-7，**无填充、无小写、无空白**，字符数 1..56。
 *
 * NOTE: 本模块只解码、不编码 —— 协议不提供种子读出（serial-protocol.md §9），
 *       设备永远不需要把种子还原成文本。
 */
#ifndef BASE32_H
#define BASE32_H

#include <stddef.h>
#include <stdint.h>

/** 种子字符数上限（56 字符 → 35 字节，serial-protocol.md §5.6） */
#define BASE32_MAX_CHARS  56U
/** 种子解码后的字节数上限 */
#define BASE32_MAX_BYTES  35U
/** 种子字符数下限 */
#define BASE32_MIN_CHARS  1U

/**
 * @brief  把 Base32 字符串解码为字节串。
 * @param  text     输入字符（仅 A-Z2-7），调用方保证 text_len 字节可读。
 * @param  text_len 字符数，须在 BASE32_MIN_CHARS..BASE32_MAX_CHARS 内。
 * @param  out      输出缓冲区，须至少 out_len 字节可写。
 * @param  out_cap  输出缓冲区容量（字节）。
 * @return 成功返回解码字节数（1..BASE32_MAX_BYTES）；输入非法或容量不足返回 -1。
 * @note   输入必须是规范编码：尾部不足 8 bit 的残留位必须为 0，否则返回 -1。
 *         单字符输入（如 "A"）只含填充位、解不出任何字节，按非法处理。
 */
int base32_decode(const char *text, size_t text_len, uint8_t *out, size_t out_cap);

#endif /* BASE32_H */
