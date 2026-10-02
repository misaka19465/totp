"""进程内设备仿真（仅测试用）：实现设备命令语义。

NOTE: 行为以 firmware/src/app/proto.c 为准，只实现测试需要的命令与故障注入，
      不参与正式工具运行。
"""

from __future__ import annotations

import struct

import protocol

PREAMBLE = b"\x00\x00\x00\x00"
INITIAL_TIME_MS = 1790899200000  # 2026-10-02T00:00:00Z
NEVER_SYNCED_AGE_S = 0xFFFFFFFF
SET_TIME_QUANTIZE_MS = 4  # 模拟 1/256 s 亚秒量化


class FakeSerial:
    """最小 pyserial 替身：写入交给 FakeDevice，回包排队给 read。"""

    def __init__(self, device: "FakeDevice") -> None:
        self.device = device
        self.written = bytearray()
        self.preamble_count = 0
        self.closed = False
        self._rx = bytearray()

    def write(self, data: bytes) -> int:
        self.written.extend(data)
        if bytes(data) == PREAMBLE:
            self.preamble_count += 1
        self._rx.extend(self.device.handle(bytes(data)))
        return len(data)

    def read(self, size: int = 1) -> bytes:
        chunk = bytes(self._rx[:size])
        del self._rx[:size]
        return chunk

    def reset_input_buffer(self) -> None:
        self._rx.clear()

    def close(self) -> None:
        self.closed = True


class FakeDevice:
    """带状态的设备模型；支持丢帧、静默、错误码与 CRC 偏差注入。"""

    def __init__(self) -> None:
        self.decoder = protocol.FrameDecoder()
        self.requests: list[protocol.Frame] = []

        self.proto_ver = protocol.PROTO_VERSION
        self.fw_ver = 0x0100
        self.max_payload = protocol.MAX_PAYLOAD
        self.uptime_s = 42
        self.reset_cause = 0
        self.battery_mv = 2900
        self.battery_level = 1

        self.time_ms = INITIAL_TIME_MS
        self.time_flags = int(protocol.TimeFlag.NEVER_SET)
        self.last_set_age_s = NEVER_SYNCED_AGE_S

        self.seed_present = False
        self.algo = protocol.ALGO_SHA1
        self.digits = protocol.DIGITS_MAX
        self.period = 30
        self.secret = ""
        self.calib_centi_ppm = 0

        self.silent = False
        self.drop_frames = 0
        self.drop_commands: set[int] = set()
        self.fail_status: dict[int, int] = {}
        self.time_bias_ms = 0
        self.crc_xor = 0
        self.meta_crc_xor = 0
        self.wrong_seq_noise = False
        self.response_noise = b""  # 在下一批回包前插入的原始字节（测试重同步）

    # ---- 链路 ----

    def handle(self, data: bytes) -> bytes:
        out = bytearray(self.response_noise)
        self.response_noise = b""
        for request in self.decoder.feed(data):
            self.requests.append(request)
            if self.silent or self.drop_frames > 0 or request.cmd in self.drop_commands:
                if self.drop_frames > 0:
                    self.drop_frames -= 1
                continue
            if self.wrong_seq_noise:
                noise = protocol.Frame(
                    request.cmd | protocol.RESPONSE_BIT, (request.seq + 1) & 0xFF, b"\x00"
                )
                out.extend(protocol.encode(noise))
            out.extend(protocol.encode(self._response(request)))
        return bytes(out)

    def _response(self, request: protocol.Frame) -> protocol.Frame:
        cmd = request.cmd | protocol.RESPONSE_BIT
        forced = self.fail_status.get(request.cmd)
        if forced is not None:
            return protocol.Frame(cmd, request.seq, bytes([forced]))
        return protocol.Frame(cmd, request.seq, bytes([protocol.Status.OK]) + self._data(request))

    # ---- 命令语义 ----

    def _data(self, request: protocol.Frame) -> bytes:
        if request.cmd == protocol.Cmd.PING:
            return (
                bytes([self.proto_ver])
                + struct.pack("<H", self.fw_ver)
                + bytes([self.max_payload])
                + struct.pack("<H", self._flags())
                + struct.pack("<I", self.uptime_s)
            )
        if request.cmd == protocol.Cmd.GET_TIME:
            return struct.pack("<Q", self.time_ms) + bytes([self.time_flags])
        if request.cmd == protocol.Cmd.SET_TIME:
            sent_ms = struct.unpack_from("<Q", request.payload)[0]
            self.time_ms = sent_ms - sent_ms % SET_TIME_QUANTIZE_MS + self.time_bias_ms
            self.time_flags = int(protocol.TimeFlag.TRUSTED)
            self.last_set_age_s = 0
            return struct.pack("<Q", self.time_ms)
        if request.cmd == protocol.Cmd.GET_STATUS:
            return (
                struct.pack("<H", self._flags())
                + bytes([self.reset_cause])
                + struct.pack("<H", self.battery_mv)
                + bytes([self.battery_level])
                + struct.pack("<I", self.last_set_age_s)
            )
        if request.cmd == protocol.Cmd.WRITE_SLOT:
            self.algo = request.payload[0]
            self.digits = request.payload[1]
            self.period = struct.unpack_from("<H", request.payload, 2)[0]
            secret_len = request.payload[4]
            self.secret = request.payload[5 : 5 + secret_len].decode("ascii")
            self.seed_present = True
            return struct.pack("<H", self._crc() ^ self.crc_xor)
        if request.cmd == protocol.Cmd.READ_META:
            if not self.seed_present:
                return b"\x00" * 8
            return (
                bytes([0x03, self.algo, self.digits])
                + struct.pack("<H", self.period)
                + bytes([len(self.secret)])
                + struct.pack("<H", self._crc() ^ self.meta_crc_xor)
            )
        if request.cmd == protocol.Cmd.ERASE_SLOT:
            self.seed_present = False
            return b""
        if request.cmd == protocol.Cmd.SET_CALIB:
            requested = struct.unpack_from("<h", request.payload)[0]
            self.calib_centi_ppm = requested
            return struct.pack("<h", requested)
        raise AssertionError(f"仿真设备未实现命令 0x{request.cmd:02X}")

    def _crc(self) -> int:
        return protocol.config_crc16(
            algo=self.algo, digits=self.digits, period=self.period, secret=self.secret
        )

    def _flags(self) -> int:
        flags = int(protocol.DeviceFlag.IWDG_ENABLED)
        if self.seed_present:
            flags |= int(protocol.DeviceFlag.SEED_PRESENT)
        if self.time_flags & int(protocol.TimeFlag.NEVER_SET):
            flags |= int(protocol.DeviceFlag.RTC_NEVER_SET)
        if self.time_flags & int(protocol.TimeFlag.STALE):
            flags |= int(protocol.DeviceFlag.RTC_STALE)
        if self.battery_level == 2:
            flags |= int(protocol.DeviceFlag.LOW_BATTERY)
        if self.battery_level == 3:
            flags |= int(protocol.DeviceFlag.CRITICAL_BATTERY)
        if self.calib_centi_ppm:
            flags |= int(protocol.DeviceFlag.CALIB_ACTIVE)
        return flags
