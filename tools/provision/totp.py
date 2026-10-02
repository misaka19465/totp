"""主机侧 TOTP 计算与 Base32 种子处理，供 setup 的屏上码比对使用。

NOTE: 与固件 app/totp.c、app/base32.c 保持同一算法与约束：
      HMAC-SHA1 + RFC 4226 动态截断；位数 1..6；Base32 无填充、
      末尾字符的填充位必须为 0（非规范写法设备会拒绝）。
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import secrets
import struct

import protocol

ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

# NOTE: 设备允许的种子为 1..35 字节（56 个 Base32 字符）；RFC 6238 的
#       HMAC-SHA1 参考密钥为 20 字节，这里作为随机种子缺省长度。
MIN_SECRET_BYTES = 1
MAX_SECRET_BYTES = protocol.MAX_SECRET_CHARS * 5 // 8
DEFAULT_SECRET_BYTES = 20


def generate_secret(num_bytes: int = DEFAULT_SECRET_BYTES) -> str:
    """生成指定字节数的随机种子，返回大写、无填充的规范 Base32。"""
    if not MIN_SECRET_BYTES <= num_bytes <= MAX_SECRET_BYTES:
        raise ValueError(f"随机种子字节数必须在 {MIN_SECRET_BYTES}..{MAX_SECRET_BYTES}")
    return base64.b32encode(secrets.token_bytes(num_bytes)).decode("ascii").rstrip("=")


def normalize_secret(raw: str) -> str:
    """清理用户输入：去空白、去填充、转大写，并校验字母表与规范尾部位。

    NOTE: 设备要求大写、无填充的规范 Base32，
          这里替操作员做等价改写，避免从密码管理器粘贴时带空格或 "="。
    """
    text = "".join(raw.split()).rstrip("=").upper()
    if not text:
        raise ValueError("种子为空")
    if len(text) > protocol.MAX_SECRET_CHARS:
        raise ValueError(f"种子 {len(text)} 个字符，超过协议上限 {protocol.MAX_SECRET_CHARS}")

    values: list[int] = []
    for char in text:
        index = ALPHABET.find(char)
        if index < 0:
            raise ValueError(f"种子含非法字符 {char!r}（只允许 A-Z 与 2-7）")
        values.append(index)

    leftover_bits = (len(text) * 5) % 8
    if leftover_bits and values[-1] & ((1 << leftover_bits) - 1):
        raise ValueError("种子不是规范 Base32（末尾字符填充位非零）")
    if (len(text) * 5) // 8 == 0:
        raise ValueError("种子过短，解不出任何字节")
    return text


def decode_secret(secret: str) -> bytes:
    """按固件 base32_decode 的语义解码（无填充、大端 5 bit 流）。"""
    text = normalize_secret(secret)
    accumulator = 0
    accumulated_bits = 0
    decoded = bytearray()
    for char in text:
        accumulator = (accumulator << 5) | ALPHABET.index(char)
        accumulated_bits += 5
        if accumulated_bits >= 8:
            accumulated_bits -= 8
            decoded.append((accumulator >> accumulated_bits) & 0xFF)
    if not decoded:
        raise ValueError("种子过短，解不出任何字节")
    return bytes(decoded)


def hotp_truncated(secret: str, counter: int) -> int:
    """RFC 4226 §5.3 动态截断后的 31 bit 整数（与固件 hotp_generate 一致）。"""
    message = struct.pack(">Q", counter)
    mac = hmac.new(decode_secret(secret), message, hashlib.sha1).digest()
    offset = mac[-1] & 0x0F
    return (
        ((mac[offset] & 0x7F) << 24)
        | (mac[offset + 1] << 16)
        | (mac[offset + 2] << 8)
        | mac[offset + 3]
    )


def generate(
    secret: str,
    *,
    unix_ms: int,
    period: int = 30,
    digits: int = protocol.DIGITS_MAX,
) -> tuple[str, int]:
    """返回 (左补零口令, 本窗口剩余秒数)；位数与周期约束同固件。"""
    if not protocol.DIGITS_MIN <= digits <= protocol.DIGITS_MAX:
        raise ValueError(f"digits 必须在 {protocol.DIGITS_MIN}..{protocol.DIGITS_MAX}")
    if not protocol.PERIOD_MIN <= period <= protocol.PERIOD_MAX:
        raise ValueError(f"period 必须在 {protocol.PERIOD_MIN}..{protocol.PERIOD_MAX} 秒")

    seconds = unix_ms // 1000
    value = hotp_truncated(secret, seconds // period) % (10**digits)
    remaining = period - seconds % period
    return f"{value:0{digits}d}", remaining
