"""protocol.py 的线格式回归测试。

NOTE: 测试向量固定线格式；改动协议必须同步更新本文件与固件测试，
      避免固件与上位机漂移。
"""

from __future__ import annotations

import struct
import unittest

import protocol as p


class CrcTests(unittest.TestCase):
    def test_ccitt_false_reference_value(self) -> None:
        self.assertEqual(p.crc16_ccitt_false(b"123456789"), 0x29B1)


class VectorTests(unittest.TestCase):
    """逐字节线格式向量。"""

    def assert_frame(self, frame: p.Frame, expected_hex: str) -> None:
        self.assertEqual(p.encode(frame).hex(" "), " ".join(expected_hex.split()))

    def test_ping_request(self) -> None:
        self.assert_frame(p.ping(0), "aa 01 00 00 ac fb")

    def test_ping_response(self) -> None:
        data = (
            bytes([1])
            + struct.pack("<H", 0x0100)
            + bytes([0x40])
            + struct.pack("<H", 0)
            + struct.pack("<I", 0)
        )
        self.assert_frame(
            p.ping(0).response(p.Status.OK, data),
            "aa 81 00 0b 00 01 00 01 40 00 00 00 00 00 00 38 6e",
        )

    def test_set_time_request_utc_ms(self) -> None:
        self.assert_frame(
            p.set_time(0x2A, unix_ms=1790899200000),
            "aa 03 2a 08 00 20 e9 f9 a0 01 00 00 86 cf",
        )

    def test_set_time_response_echoes_utc_ms(self) -> None:
        frame = p.set_time(0x2A, unix_ms=1790899200000).response(
            p.Status.OK, struct.pack("<Q", 1790899200000)
        )
        self.assert_frame(frame, "aa 83 2a 09 00 00 20 e9 f9 a0 01 00 00 ce 41")

    def test_get_status_response(self) -> None:
        data = struct.pack("<HBHBI", 0, 0, 2900, 1, 0xFFFFFFFF)
        self.assert_frame(
            p.get_status(0x2B).response(p.Status.OK, data),
            "aa 84 2b 0b 00 00 00 00 54 0b 01 ff ff ff ff 7c 2f",
        )

    def test_write_slot_request(self) -> None:
        self.assert_frame(
            p.write_slot(1, secret="JBSWY3DPEHPK3PXP", period=30),
            "aa 05 01 15 01 06 1e 00 10 4a 42 53 57 59 33 44 50 45 48 50 4b 33 50 58 50 b0 97",
        )

    def test_write_slot_request_four_digits(self) -> None:
        self.assert_frame(
            p.write_slot(3, secret="JBSWY3DPEHPK3PXP", digits=4, period=30),
            "aa 05 03 15 01 04 1e 00 10 4a 42 53 57 59 33 44 50 45 48 50 4b 33 50 58 50 c0 36",
        )
        self.assertEqual(
            p.config_crc16(algo=1, digits=4, period=30, secret="JBSWY3DPEHPK3PXP"),
            0xDD5E,
        )

    def test_write_slot_rejects_digits_out_of_range(self) -> None:
        for digits in (0, 7):
            with self.assertRaises(ValueError):
                p.write_slot(1, secret="JBSWY3DPEHPK3PXP", digits=digits)

    def test_read_meta_response(self) -> None:
        self.assertEqual(
            p.config_crc16(algo=1, digits=6, period=30, secret="JBSWY3DPEHPK3PXP"),
            0xB638,
        )
        data = (
            bytes([0x03, 1, 6])
            + struct.pack("<H", 30)
            + bytes([0x10])
            + struct.pack("<H", 0xB638)
        )
        self.assert_frame(
            p.read_meta(2).response(p.Status.OK, data),
            "aa 86 02 09 00 03 01 06 1e 00 10 38 b6 9e 9e",
        )

    def test_erase_slot_eeprom_error(self) -> None:
        self.assert_frame(
            p.erase_slot(9).response(p.Status.ERR_EEPROM),
            "aa 87 09 01 06 b3 c5",
        )


class DecoderTests(unittest.TestCase):
    def test_stream_round_trip(self) -> None:
        stream = p.encode(p.ping(1)) + p.encode(p.get_time(2))
        frames = p.FrameDecoder().feed(stream)
        self.assertEqual([(f.cmd, f.seq) for f in frames], [(0x01, 1), (0x02, 2)])

    def test_crc_error_frame_dropped_and_resync(self) -> None:
        bad = bytearray(p.encode(p.ping(1)))
        bad[-1] ^= 0xFF
        decoder = p.FrameDecoder()
        self.assertEqual(decoder.feed(bytes(bad)), [])
        frames = decoder.feed(p.encode(p.get_time(2)))
        self.assertEqual([(f.cmd, f.seq) for f in frames], [(0x02, 2)])


if __name__ == "__main__":
    unittest.main()
