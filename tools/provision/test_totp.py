"""totp.py 的 RFC 向量与种子规范化测试。"""

from __future__ import annotations

import base64
import unittest

import totp

RFC_SEED_ASCII = "12345678901234567890"
RFC_VECTORS = (  # RFC 6238 Appendix B（HMAC-SHA1，8 位）
    (59, "94287082"),
    (1111111109, "07081804"),
    (1111111111, "14050471"),
    (1234567890, "89005924"),
    (2000000000, "69279037"),
    (20000000000, "65353130"),
)


def rfc_secret() -> str:
    return base64.b32encode(RFC_SEED_ASCII.encode()).decode()


class Rfc6238Tests(unittest.TestCase):
    def test_truncated_values_match_rfc(self) -> None:
        secret = rfc_secret()
        for unix_s, expected in RFC_VECTORS:
            with self.subTest(unix_s=unix_s):
                truncated = totp.hotp_truncated(secret, unix_s // 30)
                self.assertEqual(truncated % 10**8, int(expected))

    def test_six_digit_codes_are_suffix_of_rfc_values(self) -> None:
        secret = rfc_secret()
        for unix_s, expected in RFC_VECTORS:
            with self.subTest(unix_s=unix_s):
                code, remaining = totp.generate(secret, unix_ms=unix_s * 1000)
                self.assertEqual(code, f"{int(expected) % 10**6:06d}")
                self.assertEqual(remaining, 30 - unix_s % 30)

    def test_remaining_starts_at_period_on_boundary(self) -> None:
        code, remaining = totp.generate(rfc_secret(), unix_ms=30_000, period=30)
        self.assertEqual(remaining, 30)
        self.assertEqual(len(code), 6)

    def test_digits_constraint_matches_firmware(self) -> None:
        with self.assertRaises(ValueError):
            totp.generate(rfc_secret(), unix_ms=0, digits=7)
        with self.assertRaises(ValueError):
            totp.generate(rfc_secret(), unix_ms=0, digits=0)
        with self.assertRaises(ValueError):
            totp.generate(rfc_secret(), unix_ms=0, period=301)


class SecretTests(unittest.TestCase):
    def test_normalize_strips_case_whitespace_and_padding(self) -> None:
        self.assertEqual(
            totp.normalize_secret(" jbswy3dpehpk3pxp= "), "JBSWY3DPEHPK3PXP"
        )

    def test_decode_matches_bytes(self) -> None:
        self.assertEqual(totp.decode_secret("JBSWY3DPEHPK3PXP"), b"Hello!\xde\xad\xbe\xef")

    def test_decode_accepts_firmware_style_partial_group(self) -> None:
        # 固件 base32.c 只要求末尾填充位为 0，不要求 RFC 的规范分组长度。
        self.assertEqual(totp.decode_secret("AAA"), b"\x00")

    def test_rejects_illegal_char(self) -> None:
        with self.assertRaises(ValueError):
            totp.normalize_secret("ABC1")
        with self.assertRaises(ValueError):
            totp.normalize_secret("你好")

    def test_rejects_non_canonical_padding_bits(self) -> None:
        with self.assertRaises(ValueError):
            totp.normalize_secret("AB")  # 'B' 的低 2 bit 非 0
        with self.assertRaises(ValueError):
            totp.normalize_secret("A")  # 解不出字节

    def test_rejects_empty_and_oversized(self) -> None:
        with self.assertRaises(ValueError):
            totp.normalize_secret("   ")
        with self.assertRaises(ValueError):
            totp.normalize_secret("A" * 57 + "A")  # 58 字符


class RandomSecretTests(unittest.TestCase):
    def test_default_secret_is_canonical_32_chars(self) -> None:
        secret = totp.generate_secret()
        self.assertRegex(secret, r"^[A-Z2-7]{32}$")
        self.assertEqual(len(totp.decode_secret(secret)), totp.DEFAULT_SECRET_BYTES)

    def test_requested_length_roundtrips_canonical_base32(self) -> None:
        for num_bytes in (1, 10, totp.MAX_SECRET_BYTES):
            with self.subTest(num_bytes=num_bytes):
                secret = totp.generate_secret(num_bytes)
                self.assertEqual(len(totp.decode_secret(secret)), num_bytes)
                self.assertEqual(totp.normalize_secret(secret), secret)
                self.assertNotIn("=", secret)

    def test_successive_seeds_differ(self) -> None:
        self.assertNotEqual(totp.generate_secret(), totp.generate_secret())

    def test_rejects_out_of_range_length(self) -> None:
        for num_bytes in (0, totp.MAX_SECRET_BYTES + 1):
            with self.subTest(num_bytes=num_bytes):
                with self.assertRaises(ValueError):
                    totp.generate_secret(num_bytes)


if __name__ == "__main__":
    unittest.main()
