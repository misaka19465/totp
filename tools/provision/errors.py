"""provision 工具与设备交互的异常类型。

NOTE: 所有异常都是"预期内的失败"，CLI 捕获后打印消息并以非零退出；
      只有未知状态码也要按失败处理并打印原始值（serial-protocol.md §5.9）。
"""

from __future__ import annotations

import protocol


class ProvisionError(Exception):
    """工具预期内的失败：调用方打印消息并以非零退出。"""


class DeviceError(ProvisionError):
    """设备回了非 OK 状态码（serial-protocol.md §5.9）。"""

    def __init__(self, cmd: int, status: int) -> None:
        super().__init__(f"设备返回 {protocol.status_name(status)}（命令 0x{cmd:02X}）")
        self.cmd = cmd
        self.status = status


class DeviceNotFoundError(ProvisionError):
    """前导码 + PING 多轮后设备仍无响应。"""


class DeviceTimeoutError(ProvisionError):
    """命令重传与会话重建后仍无响应。"""


class ProtocolError(ProvisionError):
    """响应帧结构或字段与规范不符。"""


class VerificationError(ProvisionError):
    """本地比对失败（config_crc16、读回参数、屏上码等）。"""
