"""session.py 的前导码、重传与会话恢复测试（进程内仿真设备）。"""

from __future__ import annotations

import unittest
from unittest import mock

import protocol
import session
from errors import DeviceNotFoundError, DeviceTimeoutError
from fake_device import FakeDevice, FakeSerial


def make_session() -> tuple[FakeDevice, FakeSerial, session.SerialSession]:
    model = FakeDevice()
    port = FakeSerial(model)
    return model, port, session.SerialSession(port)


class SessionTestBase(unittest.TestCase):
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


class WakeTests(SessionTestBase):
    def test_wake_sends_preamble_then_ping(self) -> None:
        model, port, sess = make_session()
        response = sess.wake()
        self.assertIsNotNone(response)
        self.assertEqual(port.preamble_count, 1)
        self.assertEqual([r.cmd for r in model.requests], [protocol.Cmd.PING])
        self.assertTrue(bytes(port.written).startswith(session.PREAMBLE))

    def test_wake_retries_within_round_then_next_round(self) -> None:
        model, port, sess = make_session()
        model.drop_frames = 4  # 第 1 轮 3 次全丢 + 第 2 轮第 1 次
        response = sess.wake(rounds=3, attempts=3)
        self.assertIsNotNone(response)
        self.assertEqual(port.preamble_count, 2)
        pings = [r for r in model.requests if r.cmd == protocol.Cmd.PING]
        self.assertEqual(len(pings), 5)
        self.assertEqual(len({r.seq for r in pings}), 1)  # 重传保持同一 SEQ

    def test_wake_gives_up_when_silent(self) -> None:
        model, port, sess = make_session()
        model.silent = True
        self.assertIsNone(sess.wake(rounds=2, attempts=2))
        self.assertEqual(port.preamble_count, 2)
        self.assertEqual(len(model.requests), 4)


class RequestTests(SessionTestBase):
    def test_request_retransmits_same_seq(self) -> None:
        model, port, sess = make_session()
        model.drop_frames = 2
        frame = protocol.ping(sess.next_seq())
        status, data = sess.request(frame)
        self.assertEqual(status, protocol.Status.OK)
        pings = [r for r in model.requests if r.cmd == protocol.Cmd.PING]
        self.assertEqual(len(pings), 3)
        self.assertEqual({r.seq for r in pings}, {frame.seq})

    def test_request_recovers_session_after_retries(self) -> None:
        model, port, sess = make_session()
        model.drop_frames = session.RETRIES  # 3 次命令全丢 → 重建会话
        frame = protocol.get_time(sess.next_seq())
        status, _ = sess.request(frame)
        self.assertEqual(status, protocol.Status.OK)
        self.assertEqual(port.preamble_count, 2)
        cmds = [r.cmd for r in model.requests]
        self.assertEqual(
            cmds,
            [protocol.Cmd.GET_TIME] * session.RETRIES
            + [protocol.Cmd.PING, protocol.Cmd.GET_TIME],
        )

    def test_request_raises_not_found_when_recovery_fails(self) -> None:
        model, port, sess = make_session()
        model.silent = True
        with mock.patch.multiple(session, RETRIES=1, WAKE_ROUNDS=1):
            with self.assertRaises(DeviceNotFoundError):
                sess.request(protocol.ping(sess.next_seq()))

    def test_request_raises_timeout_when_command_keeps_dropping(self) -> None:
        model, port, sess = make_session()
        # PING 始终有响应（会话可重建），但 GET_TIME 一直被丢弃。
        model.drop_commands.add(protocol.Cmd.GET_TIME)
        with self.assertRaises(DeviceTimeoutError):
            sess.request(protocol.get_time(sess.next_seq()))

    def test_idle_triggers_new_preamble(self) -> None:
        model, port, sess = make_session()
        sess.request(protocol.ping(sess.next_seq()))
        self.assertEqual(port.preamble_count, 1)
        with mock.patch.object(session, "IDLE_REPREAMBLE_S", 0.0):
            sess.request(protocol.ping(sess.next_seq()))
        self.assertEqual(port.preamble_count, 2)

    def test_ignores_wrong_seq_noise(self) -> None:
        model, port, sess = make_session()
        model.wrong_seq_noise = True
        frame = protocol.ping(sess.next_seq())
        status, data = sess.request(frame)
        self.assertEqual(status, protocol.Status.OK)
        self.assertEqual(len(data), 10)

    def test_ignores_crc_corrupted_frame_and_resyncs(self) -> None:
        model, port, sess = make_session()
        frame = protocol.get_time(sess.next_seq())
        corrupted = bytearray(
            protocol.encode(
                protocol.Frame(
                    protocol.Cmd.GET_TIME | protocol.RESPONSE_BIT,
                    frame.seq,
                    bytes([protocol.Status.OK]) + b"\x00" * 9,
                )
            )
        )
        corrupted[-1] ^= 0xFF
        model.response_noise = bytes(corrupted)  # 伪帧在前，真实回包在后
        status, _ = sess.request(frame)
        self.assertEqual(status, protocol.Status.OK)


if __name__ == "__main__":
    unittest.main()
