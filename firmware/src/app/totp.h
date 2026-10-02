/**
 * @file    totp.h
 * @brief   动态口令计算：HOTP（RFC 4226）与 TOTP（RFC 6238，HMAC-SHA1）。
 *
 * 时间输入统一为**整秒 UTC**：设备侧由 `unix_ms / 1000` 取整得到，
 * 见 docs/serial-protocol.md §5.4。
 *
 * NOTE: 位数范围 1..8 是可计算的数学上限（10^8 仍在 uint32 内，且对齐
 *       RFC 6238 的 8 位测试向量）。产品的配置位数约束是 1..6
 *       （requirements.md §4.2），由 proto/keystore 层负责校验。
 */
#ifndef TOTP_H
#define TOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 支持的最少口令位数 */
#define TOTP_DIGITS_MIN  1U
/** 支持的最多口令位数（产品配置上限 6，见文件头 NOTE） */
#define TOTP_DIGITS_MAX  8U

/**
 * @brief  计算 HOTP 动态截断值（RFC 4226 §5.3，31 bit）。
 * @param  secret        共享密钥（由 Base32 种子解码得到），长度须 > 0。
 * @param  secret_len    密钥长度（字节）。
 * @param  counter       计数器（大端 8 字节参与 HMAC）。
 * @param  truncated_out 输出截断值（0..0x7FFFFFFF）。
 * @return 成功返回 true；参数非法返回 false（不修改 *truncated_out）。
 * @note   该值尚未对 10^digits 取模；TOTP 显示用 totp_generate()。
 */
bool hotp_generate(const uint8_t *secret, size_t secret_len,
                   uint64_t counter, uint32_t *truncated_out);

/**
 * @brief  计算 TOTP 口令。
 * @param  secret        共享密钥，长度须 > 0。
 * @param  secret_len    密钥长度（字节）。
 * @param  unix_seconds  UTC 整秒。
 * @param  period_s      步长（秒），须 > 0（产品范围 15..300，见协议 §5.6）。
 * @param  digits        口令位数，须在 TOTP_DIGITS_MIN..TOTP_DIGITS_MAX 内。
 * @param  code_out      输出口令（已对 10^digits 取模，前导零由渲染层补齐）。
 * @return 成功返回 true；参数非法返回 false（不修改 *code_out）。
 */
bool totp_generate(const uint8_t *secret, size_t secret_len,
                   uint64_t unix_seconds, uint32_t period_s,
                   uint8_t digits, uint32_t *code_out);

/**
 * @brief  当前口令的剩余有效秒数（含当前秒）。
 * @param  unix_seconds UTC 整秒。
 * @param  period_s     步长（秒）；为 0 时返回 0。
 * @return period_s - (unix_seconds mod period_s)，取值范围 1..period_s。
 * @note   配置工具用它决定"屏上码比对"的时机（serial-protocol.md §6）。
 */
uint32_t totp_seconds_remaining(uint64_t unix_seconds, uint32_t period_s);

/**
 * @brief  下一个 TOTP 窗口边界的 UTC 毫秒时刻。
 * @param  unix_ms  当前 UTC 毫秒。
 * @param  period_s 步长（秒）；为 0 时返回 0。
 * @return 严格大于 unix_ms 的窗口边界毫秒值（对齐到整秒）。
 * @note   主循环用它把唤醒闹钟精确设在口令更新时刻（architecture.md §4.4）。
 */
uint64_t totp_next_boundary_ms(uint64_t unix_ms, uint32_t period_s);

#endif /* TOTP_H */
