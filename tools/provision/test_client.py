"""client.py 的命令解析、写后读回与错误处理测试。"""

from __future__ import annotations

import unittest
from unittest import mock

import client
import protocol
import session
from errors import DeviceError, ProtocolError, VerificationError
from fake_device import FakeDevice, FakeSerial

TEST_SECRET = "JBSWY3DPEHPK3PXP"
TEST_CRC = 0xB638


class ClientTestBase(unittest.TestCase):
    def setUp(self) -> None:
        patcher = mock.patch.multiple(
            session,
            PREAMBLE_SILENCE_S=0.001,
            TIMEOUT_FAST_S=0.05,
            TIMEOUT_STORAGE_S=0.05,
            POLL_INTERVAL_S=0.0005,
        )
        patcher.start()
        self.addCleanup(patcher.stop)

        self.model = FakeDevice()
        self.port = FakeSerial(self.model)
        self.device = client.Device(session.SerialSession(self.port))
        self.addCleanup(self.device.close)

    def write_test_seed(self, **kwargs: object) -> int:
        return self.device.write_slot(secret=TEST_SECRET, **kwargs)  # type: ignore[arg-type]


class OpenTests(ClientTestBase):
    def test_open_parses_ping(self) -> None:
        info = self.device.open()
        self.assertEqual(info.proto_ver, protocol.PROTO_VERSION)
        self.assertEqual(info.fw_version, "1.0")
        self.assertEqual(info.max_payload, protocol.MAX_PAYLOAD)
        self.assertFalse(info.seed_present)
        self.assertTrue(info.flags & protocol.DeviceFlag.IWDG_ENABLED)
        client.check_compatible(info)  # 不抛异常

    def test_check_compatible_rejects_old_protocol(self) -> None:
        self.model.proto_ver = 2
        info = self.device.open()
        with self.assertRaises(ProtocolError):
            client.check_compatible(info)

    def test_check_compatible_rejects_small_payload(self) -> None:
        self.model.max_payload = 60
        info = self.device.open()
        with self.assertRaises(ProtocolError):
            client.check_compatible(info)

    def test_ping_after_open_reuses_session(self) -> None:
        self.device.open()
        info = self.device.ping()
        self.assertEqual(info.uptime_s, self.model.uptime_s)
        self.assertEqual(self.port.preamble_count, 1)


class TimeTests(ClientTestBase):
    def test_get_time(self) -> None:
        self.device.open()
        reading = self.device.get_time()
        self.assertEqual(reading.unix_ms, self.model.time_ms)
        self.assertFalse(reading.trusted)

    def test_set_time_within_tolerance_updates_device(self) -> None:
        self.device.open()
        sent_ms, readback_ms = self.device.set_time()
        self.assertLessEqual(abs(readback_ms - sent_ms), client.TIME_SYNC_TOLERANCE_MS)
        self.assertEqual(self.model.time_flags, int(protocol.TimeFlag.TRUSTED))
        self.assertAlmostEqual(self.model.time_ms / 1000, sent_ms / 1000, delta=0.1)

    def test_set_time_retries_and_rejects_large_bias(self) -> None:
        self.model.time_bias_ms = client.TIME_SYNC_TOLERANCE_MS + 50
        with self.assertRaises(VerificationError):
            self.device.set_time()
        set_time_frames = [r for r in self.model.requests if r.cmd == protocol.Cmd.SET_TIME]
        self.assertEqual(len(set_time_frames), client.TIME_SYNC_ATTEMPTS)


class ConfigTests(ClientTestBase):
    def test_status_snapshot(self) -> None:
        self.device.open()
        snapshot = self.device.status()
        self.assertEqual(snapshot.battery_mv, self.model.battery_mv)
        self.assertEqual(snapshot.battery_level, 1)
        self.assertTrue(snapshot.never_synced)

    def test_write_slot_normalizes_secret_and_checks_crc(self) -> None:
        self.device.open()
        crc = self.device.write_slot(secret=" jbswy3dpehpk3pxp= ", digits=6, period=30)
        self.assertEqual(crc, TEST_CRC)
        self.assertEqual(self.model.secret, TEST_SECRET)  # 大写、无填充
        self.assertTrue(self.model.seed_present)

    def test_write_slot_crc_mismatch_raises(self) -> None:
        self.model.crc_xor = 0x0001
        with self.assertRaises(VerificationError):
            self.write_test_seed()

    def test_read_meta_round_trip(self) -> None:
        self.device.open()
        self.write_test_seed(digits=4, period=15)
        meta = self.device.read_meta()
        self.assertTrue(meta.seed_present)
        self.assertTrue(meta.record_valid)
        self.assertEqual(meta.algo, protocol.ALGO_SHA1)
        self.assertEqual(meta.digits, 4)
        self.assertEqual(meta.period, 15)
        self.assertEqual(meta.secret_len, len(TEST_SECRET))
        self.assertEqual(
            meta.config_crc16,
            protocol.config_crc16(algo=1, digits=4, period=15, secret=TEST_SECRET),
        )

    def test_read_meta_empty(self) -> None:
        self.device.open()
        meta = self.device.read_meta()
        self.assertFalse(meta.seed_present)
        self.assertEqual(meta.flags, 0)

    def test_erase_slot_clears_meta(self) -> None:
        self.device.open()
        self.write_test_seed()
        self.device.erase_slot()
        meta = self.device.read_meta()
        self.assertEqual(meta.flags, 0)
        self.assertFalse(self.model.seed_present)

    def test_set_calib_returns_applied_value(self) -> None:
        self.device.open()
        self.assertEqual(self.device.set_calib(-1250), -1250)
        self.assertEqual(self.model.calib_centi_ppm, -1250)

    def test_device_error_status_is_reported(self) -> None:
        self.model.fail_status[protocol.Cmd.ERASE_SLOT] = protocol.Status.ERR_EEPROM
        with self.assertRaises(DeviceError) as ctx:
            self.device.erase_slot()
        self.assertEqual(ctx.exception.status, protocol.Status.ERR_EEPROM)
        self.assertIn("ERR_EEPROM", str(ctx.exception))

    def test_unknown_status_prints_raw_value(self) -> None:
        self.model.fail_status[protocol.Cmd.ERASE_SLOT] = 0x42
        with self.assertRaises(DeviceError) as ctx:
            self.device.erase_slot()
        self.assertIn("UNKNOWN(0x42)", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
