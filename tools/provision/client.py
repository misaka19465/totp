"""设备命令层：字段校验、类型化封装与写后读回确认。

NOTE: 本层把帧级会话（session.py）包装成有类型、有长度校验的命令调用；
      所有"设备回了什么才算成功"的判断都在这里，CLI 只负责展示与流程编排。
"""

from __future__ import annotations

import dataclasses
import struct
import time

import protocol
import session as session_mod
import totp
from errors import DeviceError, DeviceNotFoundError, ProtocolError, VerificationError

PING_DATA_LEN = 10
TIME_DATA_LEN = 9
SET_TIME_DATA_LEN = 8
STATUS_DATA_LEN = 10
META_DATA_LEN = 8
CALIB_DATA_LEN = 2

MIN_MAX_PAYLOAD = 61
SEED_PRESENT_BIT = 1 << 0
RECORD_VALID_BIT = 1 << 1
NEVER_SYNCED_AGE_S = 0xFFFFFFFF

# NOTE: 实机实测（2026-10-02，L031 实物）SET_TIME 立即回读偏大 10~20 ms，
#       稳定后偏差为负（单向链路时延量级）；对 30 s 的 TOTP 窗口余量足够。
#       容差取值与固件内部 CLOCK_RTC_SET_TOLERANCE_MS 一致。
TIME_SYNC_TOLERANCE_MS = 100
TIME_SYNC_ATTEMPTS = 3


def _u16(data: bytes) -> int:
    return struct.unpack_from("<H", data)[0]


def _u32(data: bytes) -> int:
    return struct.unpack_from("<I", data)[0]


def _u64(data: bytes) -> int:
    return struct.unpack_from("<Q", data)[0]


def _i16(data: bytes) -> int:
    return struct.unpack_from("<h", data)[0]


@dataclasses.dataclass(frozen=True)
class DeviceInfo:
    """PING 响应。"""

    proto_ver: int
    fw_major: int
    fw_minor: int
    max_payload: int
    flags: int
    uptime_s: int

    @property
    def fw_version(self) -> str:
        return f"{self.fw_major}.{self.fw_minor}"

    @property
    def seed_present(self) -> bool:
        return bool(self.flags & protocol.DeviceFlag.SEED_PRESENT)


@dataclasses.dataclass(frozen=True)
class TimeReading:
    """GET_TIME / SET_TIME 回读。"""

    unix_ms: int
    flags: int

    @property
    def trusted(self) -> bool:
        return bool(self.flags & protocol.TimeFlag.TRUSTED)


@dataclasses.dataclass(frozen=True)
class StatusSnapshot:
    """GET_STATUS 响应。"""

    flags: int
    reset_cause: int
    battery_mv: int
    battery_level: int
    last_set_age_s: int

    @property
    def never_synced(self) -> bool:
        return self.last_set_age_s == NEVER_SYNCED_AGE_S


@dataclasses.dataclass(frozen=True)
class Meta:
    """READ_META 响应。"""

    flags: int
    algo: int
    digits: int
    period: int
    secret_len: int
    config_crc16: int

    @property
    def seed_present(self) -> bool:
        return bool(self.flags & SEED_PRESENT_BIT)

    @property
    def record_valid(self) -> bool:
        return bool(self.flags & RECORD_VALID_BIT)


def check_compatible(info: DeviceInfo) -> None:
    """proto_ver == 1 且 max_payload ≥ 61，否则拒绝继续。"""
    if info.proto_ver != protocol.PROTO_VERSION:
        raise ProtocolError(
            f"设备协议版本为 v{info.proto_ver}，本工具只支持 v{protocol.PROTO_VERSION}"
        )
    if info.max_payload < MIN_MAX_PAYLOAD:
        raise ProtocolError(
            f"设备最大载荷 {info.max_payload} B 小于 {MIN_MAX_PAYLOAD} B，无法写入最长种子"
        )


def _require_len(cmd: protocol.Cmd, data: bytes, expected: int) -> None:
    if len(data) != expected:
        raise ProtocolError(f"命令 0x{cmd:02X} 响应数据 {len(data)} B，期望 {expected} B")


class Device:
    """面向单台设备的命令客户端；每次 CLI 调用对应一个实例。"""

    def __init__(self, port_session: session_mod.SerialSession) -> None:
        self._session = port_session
        self._info: DeviceInfo | None = None

    @property
    def info(self) -> DeviceInfo | None:
        return self._info

    def close(self) -> None:
        self._session.close()

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def _request_ok(self, frame: protocol.Frame, *, storage: bool = False) -> bytes:
        status, data = self._session.request(frame, storage=storage)
        if status != protocol.Status.OK:
            raise DeviceError(frame.cmd, status)
        return data

    def open(self, *, quick: bool = False) -> DeviceInfo:
        """前导码 + PING 握手；quick 供自动探测用（1 轮 2 次，减少等待）。"""
        if quick:
            response = self._session.wake(rounds=1, attempts=2)
        else:
            response = self._session.wake()
        if response is None:
            raise DeviceNotFoundError("设备无响应（已发送前导码与 PING，请检查接线、串口号与设备电源）")
        info = self._parse_ping(*response)
        self._info = info
        return info

    def ping(self) -> DeviceInfo:
        frame = protocol.ping(self._session.next_seq())
        return self._parse_ping(*self._session.request(frame))

    @staticmethod
    def _parse_ping(status: int, data: bytes) -> DeviceInfo:
        if status != protocol.Status.OK:
            raise DeviceError(protocol.Cmd.PING, status)
        _require_len(protocol.Cmd.PING, data, PING_DATA_LEN)
        fw_ver = _u16(data[1:3])
        return DeviceInfo(
            proto_ver=data[0],
            fw_major=fw_ver >> 8,
            fw_minor=fw_ver & 0xFF,
            max_payload=data[3],
            flags=_u16(data[4:6]),
            uptime_s=_u32(data[6:10]),
        )

    def get_time(self) -> TimeReading:
        frame = protocol.get_time(self._session.next_seq())
        data = self._request_ok(frame)
        _require_len(protocol.Cmd.GET_TIME, data, TIME_DATA_LEN)
        return TimeReading(unix_ms=_u64(data), flags=data[8])

    def set_time(self, *, max_attempts: int | None = None) -> tuple[int, int]:
        """发送前采样 UTC 毫秒并回读确认（容差 ±100 ms）。

        NOTE: 每次重试都重新采样时间；重传旧时间戳只会把设备时间写回过去。
        """
        max_attempts = TIME_SYNC_ATTEMPTS if max_attempts is None else max_attempts
        last_delta: int | None = None
        for _ in range(max_attempts):
            sent_ms = time.time_ns() // 1_000_000
            frame = protocol.set_time(self._session.next_seq(), unix_ms=sent_ms)
            data = self._request_ok(frame)
            _require_len(protocol.Cmd.SET_TIME, data, SET_TIME_DATA_LEN)
            readback_ms = _u64(data)
            if abs(readback_ms - sent_ms) <= TIME_SYNC_TOLERANCE_MS:
                return sent_ms, readback_ms
            last_delta = readback_ms - sent_ms
        raise VerificationError(
            f"对时回读误差 {last_delta:+d} ms，超过 ±{TIME_SYNC_TOLERANCE_MS} ms"
        )

    def status(self) -> StatusSnapshot:
        frame = protocol.get_status(self._session.next_seq())
        data = self._request_ok(frame)
        _require_len(protocol.Cmd.GET_STATUS, data, STATUS_DATA_LEN)
        return StatusSnapshot(
            flags=_u16(data[0:2]),
            reset_cause=data[2],
            battery_mv=_u16(data[3:5]),
            battery_level=data[5],
            last_set_age_s=_u32(data[6:10]),
        )

    def write_slot(
        self,
        *,
        secret: str,
        digits: int = protocol.DIGITS_MAX,
        period: int = 30,
    ) -> int:
        """写种子并对响应里的 config_crc16 做本地复算比对。"""
        secret = totp.normalize_secret(secret)
        expected_crc = protocol.config_crc16(
            algo=protocol.ALGO_SHA1, digits=digits, period=period, secret=secret
        )
        frame = protocol.write_slot(
            self._session.next_seq(), secret=secret, digits=digits, period=period
        )
        data = self._request_ok(frame, storage=True)
        _require_len(protocol.Cmd.WRITE_SLOT, data, 2)
        readback_crc = _u16(data)
        if readback_crc != expected_crc:
            raise VerificationError(
                f"WRITE_SLOT 回读 config_crc16=0x{readback_crc:04X}，"
                f"本地复算=0x{expected_crc:04X}"
            )
        return readback_crc

    def read_meta(self) -> Meta:
        frame = protocol.read_meta(self._session.next_seq())
        data = self._request_ok(frame)
        _require_len(protocol.Cmd.READ_META, data, META_DATA_LEN)
        return Meta(
            flags=data[0],
            algo=data[1],
            digits=data[2],
            period=_u16(data[3:5]),
            secret_len=data[5],
            config_crc16=_u16(data[6:8]),
        )

    def erase_slot(self) -> None:
        frame = protocol.erase_slot(self._session.next_seq())
        data = self._request_ok(frame, storage=True)
        _require_len(protocol.Cmd.ERASE_SLOT, data, 0)

    def set_calib(self, centi_ppm: int) -> int:
        frame = protocol.set_calib(self._session.next_seq(), cal_centi_ppm=centi_ppm)
        data = self._request_ok(frame, storage=True)
        _require_len(protocol.Cmd.SET_CALIB, data, CALIB_DATA_LEN)
        return _i16(data)
