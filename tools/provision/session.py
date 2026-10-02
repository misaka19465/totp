"""耳机口串口会话：前导码、帧收发、会话恢复。

NOTE: 线格式定义在 protocol.py；本模块只负责把一帧送达设备并取回响应，
      不做任何命令语义校验。
"""

from __future__ import annotations

import time
from typing import Callable, Protocol

import protocol
from errors import DeviceNotFoundError, DeviceTimeoutError, ProtocolError, ProvisionError

PREAMBLE = b"\x00\x00\x00\x00"
PREAMBLE_SILENCE_S = 0.025
IDLE_REPREAMBLE_S = 5.0
TIMEOUT_FAST_S = 0.3
TIMEOUT_STORAGE_S = 0.5
RETRIES = 3
WAKE_ROUNDS = 3
POLL_INTERVAL_S = 0.002
READ_CHUNK = 64


class SerialLike(Protocol):
    """SerialSession 需要的最小串口能力（pyserial 或测试替身）。"""

    def write(self, data: bytes) -> int | None: ...

    def read(self, size: int = 1) -> bytes: ...

    def reset_input_buffer(self) -> None: ...

    def close(self) -> None: ...


def open_serial(port: str) -> SerialLike:
    """以协议固定的 115200 8N1 打开串口（非阻塞读，超时由会话层控制）。"""
    try:
        import serial
    except ImportError as exc:
        raise ProvisionError("缺少 pyserial，请先安装：python3 -m pip install pyserial") from exc

    try:
        return serial.Serial(
            port=port,
            baudrate=115200,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0,
            write_timeout=1.0,
        )
    except serial.SerialException as exc:
        raise ProvisionError(f"无法打开串口 {port}：{exc}") from exc


class SerialSession:
    """一次串口会话：管理 SEQ、唤醒前导、超时与重传。"""

    def __init__(self, port: SerialLike, *, trace: Callable[[str], None] | None = None) -> None:
        self._port = port
        self._trace = trace
        self._seq = 0
        self._last_activity: float | None = None

    def close(self) -> None:
        self._port.close()

    def next_seq(self) -> int:
        """上位机自增（mod 256），重传保持同一 SEQ。"""
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return seq

    def _log(self, message: str) -> None:
        if self._trace is not None:
            self._trace(message)

    def _preamble(self) -> None:
        """4 B 前导码 + ≥20 ms 静默。"""
        self._log(f"发送唤醒前导码（{PREAMBLE.hex(' ')}）+ {PREAMBLE_SILENCE_S * 1000:.0f} ms 静默")
        self._port.reset_input_buffer()
        self._port.write(PREAMBLE)
        time.sleep(PREAMBLE_SILENCE_S)
        self._last_activity = None

    def _exchange(self, frame: protocol.Frame, timeout_s: float) -> tuple[int, bytes] | None:
        """发送一帧并等待匹配响应；超时返回 None。"""
        wire = protocol.encode(frame)
        self._log(f"发送：{wire.hex(' ')}")
        self._port.reset_input_buffer()
        self._port.write(wire)

        deadline = time.monotonic() + timeout_s
        decoder = protocol.FrameDecoder()
        expected_cmd = frame.cmd | protocol.RESPONSE_BIT
        while True:
            chunk = self._port.read(READ_CHUNK)
            if chunk:
                for response in decoder.feed(chunk):
                    if response.cmd == expected_cmd and response.seq == frame.seq:
                        self._last_activity = time.monotonic()
                        self._log(f"收到：{protocol.encode(response).hex(' ')}")
                        try:
                            return protocol.parse_response(response)
                        except ValueError as exc:
                            raise ProtocolError(str(exc)) from exc
                    self._log(f"忽略不匹配帧：cmd=0x{response.cmd:02X} seq={response.seq}")
            if time.monotonic() >= deadline:
                return None
            if not chunk:
                time.sleep(POLL_INTERVAL_S)

    def wake(self, *, rounds: int | None = None, attempts: int | None = None) -> tuple[int, bytes] | None:
        """会话建立：前导码 + PING，最多 rounds 轮、每轮 attempts 次。"""
        rounds = WAKE_ROUNDS if rounds is None else rounds
        attempts = RETRIES if attempts is None else attempts
        ping = protocol.ping(self.next_seq())
        for _ in range(rounds):
            self._preamble()
            for _ in range(attempts):
                response = self._exchange(ping, TIMEOUT_FAST_S)
                if response is not None:
                    return response
        return None

    def request(self, frame: protocol.Frame, *, storage: bool = False) -> tuple[int, bytes]:
        """发送命令并取响应；重传 3 次，仍失败则重建会话后再试。"""
        timeout_s = TIMEOUT_STORAGE_S if storage else TIMEOUT_FAST_S
        if self._last_activity is None or time.monotonic() - self._last_activity > IDLE_REPREAMBLE_S:
            self._preamble()

        for _ in range(RETRIES):
            response = self._exchange(frame, timeout_s)
            if response is not None:
                return response

        self._log("命令连续无响应，重新建立会话")
        if self.wake() is None:
            raise DeviceNotFoundError("设备无响应（已重发前导码与 PING，请检查接线、串口号与设备电源）")

        for _ in range(RETRIES):
            response = self._exchange(frame, timeout_s)
            if response is not None:
                return response
        raise DeviceTimeoutError(f"命令 0x{frame.cmd:02X}（seq={frame.seq}）重传后仍无响应")
