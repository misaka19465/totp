#!/usr/bin/env python3
"""打卡点动态码牌配置工具（docs/serial-protocol.md §6 的命令行实现）。

用法示例：
  provision.py -p /dev/ttyUSB0 ping
  provision.py -p /dev/ttyUSB0 set-time
  provision.py -p /dev/ttyUSB0 write-seed JBSWY3DPEHPK3PXP --digits 6 --period 30
  provision.py -p /dev/ttyUSB0 verify JBSWY3DPEHPK3PXP --yes

NOTE: 依赖 pyserial（`python3 -m pip install pyserial`）；缺省 --port 时
      自动探测 USB 串口，也可用 TOTP_PORT 环境变量指定。
"""

from __future__ import annotations

import argparse
import datetime
import enum
import os
import sys
import time
from typing import Callable, Sequence

import protocol
import session as session_mod
import totp
from client import (
    TIME_SYNC_TOLERANCE_MS,
    Device,
    DeviceInfo,
    Meta,
    StatusSnapshot,
    check_compatible,
)
from errors import ProvisionError, VerificationError

VERIFY_MIN_REMAINING_S = 5

RESET_CAUSES = {
    0: "POR/BOR",
    1: "IWDG（看门狗）",
    2: "软件复位",
    3: "其它",
    4: "未知",
}

BATTERY_LEVELS = {
    0: "未知",
    1: "正常",
    2: "低电",
    3: "极低",
}


def _now_ms() -> int:
    return time.time_ns() // 1_000_000


def _fmt_time(ms: int, tz: datetime.tzinfo) -> str:
    moment = datetime.datetime.fromtimestamp(ms / 1000, tz=tz)
    return moment.strftime("%Y-%m-%d %H:%M:%S.") + f"{moment.microsecond // 1000:03d}"


def _fmt_local_ms(ms: int) -> str:
    return _fmt_time(ms, datetime.datetime.now().astimezone().tzinfo)


def _fmt_duration(seconds: int) -> str:
    if seconds < 60:
        return f"{seconds} 秒"
    if seconds < 3600:
        return f"{seconds // 60} 分 {seconds % 60} 秒"
    if seconds < 86400:
        return f"{seconds // 3600} 小时 {(seconds % 3600) // 60} 分"
    return f"{seconds // 86400} 天 {(seconds % 86400) // 3600} 小时"


def _decode_flags(flags: int, enum_cls: type[enum.IntFlag]) -> str:
    names: list[str] = []
    known = 0
    for member in enum_cls:
        if flags & int(member):
            names.append(member.name)
            known |= int(member)
    unknown = flags & ~known
    if unknown:
        names.append(f"UNKNOWN(0x{unknown:04X})")
    return " | ".join(names) if names else "无"


def _connect(device: Device) -> DeviceInfo:
    info = device.info if device.info is not None else device.open()
    check_compatible(info)
    return info


def _assert_meta_matches(meta: Meta, *, secret: str, digits: int, period: int) -> None:
    """规范 §6 第 4 步：READ_META 的字段与 config_crc16 必须与写入一致。"""
    expected_crc = protocol.config_crc16(
        algo=protocol.ALGO_SHA1, digits=digits, period=period, secret=secret
    )
    problems: list[str] = []
    if not meta.seed_present:
        problems.append("READ_META 报告未写入种子")
    else:
        if not meta.record_valid:
            problems.append("记录校验位未通过")
        if meta.algo != protocol.ALGO_SHA1:
            problems.append(f"algo={meta.algo}，期望 {protocol.ALGO_SHA1}")
        if meta.digits != digits:
            problems.append(f"digits={meta.digits}，期望 {digits}")
        if meta.period != period:
            problems.append(f"period={meta.period}，期望 {period}")
        if meta.secret_len != len(secret):
            problems.append(f"secret_len={meta.secret_len}，期望 {len(secret)}")
        if meta.config_crc16 != expected_crc:
            problems.append(
                f"config_crc16=0x{meta.config_crc16:04X}，期望 0x{expected_crc:04X}"
            )
    if problems:
        raise VerificationError("读回配置与写入不一致：" + "；".join(problems))


def _code_for_screen(secret: str, *, period: int, digits: int) -> tuple[str, int]:
    """算出口令；剩余不足 5 s 时等到下一个窗口，避免屏上码跳变。"""
    code, remaining = totp.generate(secret, unix_ms=_now_ms(), period=period, digits=digits)
    for _ in range(2):
        if remaining >= VERIFY_MIN_REMAINING_S:
            break
        time.sleep(remaining + 0.1)
        code, remaining = totp.generate(secret, unix_ms=_now_ms(), period=period, digits=digits)
    return code, remaining


def _ask_screen_match(*, assume_yes: bool) -> bool | None:
    """返回 True=一致、False=不一致、None=跳过人工比对。"""
    if assume_yes:
        print("（--yes：跳过屏上码人工比对）")
        return None
    if not sys.stdin.isatty():
        print("（非交互环境：跳过屏上码人工比对，可用 --yes 显式跳过）")
        return None
    answer = input("设备屏上数字与口令一致？[y/N] ").strip().lower()
    return answer in ("y", "yes")


def _print_status(snapshot: StatusSnapshot) -> None:
    print(f"状态位：  {_decode_flags(snapshot.flags, protocol.DeviceFlag)}")
    print(f"复位原因：{RESET_CAUSES.get(snapshot.reset_cause, f'未知({snapshot.reset_cause})')}")
    print(f"电量：    {snapshot.battery_mv} mV（{BATTERY_LEVELS.get(snapshot.battery_level, '未知')}）")
    if snapshot.never_synced:
        print("距上次对时：从未对时")
    else:
        print(f"距上次对时：{_fmt_duration(snapshot.last_set_age_s)}")


# ---- 各子命令 ----


def _cmd_ping(device: Device, args: argparse.Namespace) -> int:
    info = device.open()
    print(f"协议版本：v{info.proto_ver}")
    print(f"固件版本：{info.fw_version}")
    print(f"最大载荷：{info.max_payload} B")
    print(f"运行时长：{_fmt_duration(info.uptime_s)}")
    print(f"状态位：  {_decode_flags(info.flags, protocol.DeviceFlag)}")
    check_compatible(info)
    return 0


def _cmd_get_time(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    reading = device.get_time()
    print(f"设备时间：{_fmt_time(reading.unix_ms, datetime.timezone.utc)} UTC")
    print(f"本地时间：{_fmt_local_ms(reading.unix_ms)}")
    print(f"时间标志：{_decode_flags(reading.flags, protocol.TimeFlag)}")
    print(f"与主机差值：{reading.unix_ms - _now_ms():+d} ms（粗略，含传输延迟）")
    return 0


def _cmd_set_time(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    sent_ms, readback_ms = device.set_time()
    print(f"对时完成：{_fmt_time(readback_ms, datetime.timezone.utc)} UTC")
    print(f"回读误差：{readback_ms - sent_ms:+d} ms（容差 ±{TIME_SYNC_TOLERANCE_MS} ms）")
    return 0


def _cmd_status(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    _print_status(device.status())
    return 0


def _cmd_read_meta(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    meta = device.read_meta()
    if not meta.seed_present:
        print("未写入种子（READ_META flags=0x00）")
        return 0
    algo = "HMAC-SHA1" if meta.algo == protocol.ALGO_SHA1 else f"未知（algo={meta.algo}）"
    print(f"种子：已写入，{meta.secret_len} 个 Base32 字符")
    print(f"算法：{algo}")
    print(f"位数：{meta.digits}（周期 {meta.period} s）")
    print(f"config_crc16：0x{meta.config_crc16:04X}")
    if not meta.record_valid:
        print("警告：记录校验位未通过")
    return 0


def _cmd_write_seed(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    crc = device.write_slot(secret=args.secret, digits=args.digits, period=args.period)
    meta = device.read_meta()
    _assert_meta_matches(meta, secret=args.secret, digits=args.digits, period=args.period)
    print(
        f"写入并读回确认：digits={meta.digits}，period={meta.period} s，"
        f"种子 {meta.secret_len} 字符，config_crc16=0x{crc:04X}"
    )
    return 0


def _cmd_erase(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    device.erase_slot()
    meta = device.read_meta()
    if meta.flags != 0:
        raise VerificationError(f"擦除后 READ_META flags=0x{meta.flags:02X}，仍报告配置存在")
    print("已擦除：READ_META 确认配置为空")
    return 0


def _cmd_set_calib(device: Device, args: argparse.Namespace) -> int:
    _connect(device)
    applied = device.set_calib(args.centi_ppm)
    if applied == args.centi_ppm:
        print(f"校准已写入并生效：{applied} ×0.01 ppm")
    else:
        print(
            f"校准已写入：请求 {args.centi_ppm} ×0.01 ppm，"
            f"实际生效 {applied} ×0.01 ppm（最接近档位）"
        )
    return 0


def _cmd_verify(device: Device, args: argparse.Namespace) -> int:
    """规范 §6 的完整配置流程。"""
    info = _connect(device)
    print(f"[1/5] 握手：OK（协议 v{info.proto_ver}，固件 {info.fw_version}）")

    sent_ms, readback_ms = device.set_time()
    print(f"[2/5] 对时：OK（回读误差 {readback_ms - sent_ms:+d} ms）")

    crc = device.write_slot(secret=args.secret, digits=args.digits, period=args.period)
    print(f"[3/5] 写入种子：OK（config_crc16=0x{crc:04X}）")

    meta = device.read_meta()
    _assert_meta_matches(meta, secret=args.secret, digits=args.digits, period=args.period)
    print(
        f"[4/5] 读回比对：OK（digits={meta.digits}，period={meta.period} s，"
        f"种子 {meta.secret_len} 字符）"
    )

    code, remaining = _code_for_screen(args.secret, period=meta.period, digits=meta.digits)
    hint = "（屏上右对齐显示右侧数字）" if meta.digits < protocol.DIGITS_MAX else ""
    print(f"[5/5] 屏上码比对：当前口令 {code}，剩余 {remaining} s {hint}".rstrip())
    confirmed = _ask_screen_match(assume_yes=args.yes)
    if confirmed is True:
        print("已人工确认屏上数字一致。")

    _print_status(device.status())
    if confirmed is False:
        raise VerificationError("操作员报告屏上数字与口令不一致")
    print("配置完成。")
    return 0


# ---- 参数校验（连接设备前执行，失败按用法错误退出 2） ----


def _validate_write_seed(args: argparse.Namespace) -> None:
    args.secret = totp.normalize_secret(args.secret)
    if not protocol.DIGITS_MIN <= args.digits <= protocol.DIGITS_MAX:
        raise ValueError(f"--digits 必须在 {protocol.DIGITS_MIN}..{protocol.DIGITS_MAX}")
    if not protocol.PERIOD_MIN <= args.period <= protocol.PERIOD_MAX:
        raise ValueError(f"--period 必须在 {protocol.PERIOD_MIN}..{protocol.PERIOD_MAX} 秒")


def _validate_set_calib(args: argparse.Namespace) -> None:
    limit = protocol.CAL_CENTI_PPM_MAX
    if not -limit <= args.centi_ppm <= limit:
        raise ValueError(f"校准值必须在 ±{limit}（0.01 ppm）内")


def _add_seed_arguments(parser: argparse.ArgumentParser, *, verify: bool = False) -> None:
    parser.add_argument("secret", help="Base32 种子（A-Z 与 2-7；可带空格与填充，写入前规范化）")
    parser.add_argument(
        "--digits",
        type=int,
        default=protocol.DIGITS_MAX,
        help=f"口令位数 {protocol.DIGITS_MIN}..{protocol.DIGITS_MAX}（默认 {protocol.DIGITS_MAX}）",
    )
    parser.add_argument(
        "--period",
        type=int,
        default=30,
        help=f"口令周期秒数 {protocol.PERIOD_MIN}..{protocol.PERIOD_MAX}（默认 30）",
    )
    if verify:
        parser.add_argument("-y", "--yes", action="store_true", help="跳过屏上码人工比对（脚本用）")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="provision.py",
        description="打卡点动态码牌配置工具（串口协议 v1.2，115200 8N1）",
        epilog=(
            "示例：\n"
            "  provision.py -p /dev/ttyUSB0 ping\n"
            "  provision.py -p /dev/ttyUSB0 set-time\n"
            "  provision.py -p /dev/ttyUSB0 write-seed JBSWY3DPEHPK3PXP --digits 6\n"
            "  provision.py -p /dev/ttyUSB0 verify JBSWY3DPEHPK3PXP --yes\n"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-p",
        "--port",
        help="串口设备；缺省时自动探测 USB 串口（也可用 TOTP_PORT 环境变量）",
    )
    parser.add_argument("-v", "--verbose", action="count", default=0, help="打印串口收发帧（可重复）")

    sub = parser.add_subparsers(dest="command", required=True, metavar="命令")

    ping = sub.add_parser("ping", help="连通性与版本握手")
    ping.set_defaults(handler=_cmd_ping)

    get_time = sub.add_parser("get-time", help="读取设备 UTC 时间与可信标志")
    get_time.set_defaults(handler=_cmd_get_time)

    set_time = sub.add_parser("set-time", help="以主机 UTC 时间对时（回读误差 ≤100 ms）")
    set_time.set_defaults(handler=_cmd_set_time)

    status = sub.add_parser("status", help="读取电量/对时/复位/故障快照")
    status.set_defaults(handler=_cmd_status)

    read_meta = sub.add_parser("read-meta", help="读取配置元信息（不回显种子）")
    read_meta.set_defaults(handler=_cmd_read_meta)

    write_seed = sub.add_parser("write-seed", help="写入种子并读回比对")
    _add_seed_arguments(write_seed)
    write_seed.set_defaults(handler=_cmd_write_seed, validate=_validate_write_seed)

    erase = sub.add_parser("erase", help="删除种子并确认记录为空")
    erase.set_defaults(handler=_cmd_erase)

    calib = sub.add_parser("set-calib", help="写 RTC 校准（0.01 ppm，0 = 关闭）")
    calib.add_argument(
        "centi_ppm",
        type=int,
        help=f"校准值，{-protocol.CAL_CENTI_PPM_MAX}..{protocol.CAL_CENTI_PPM_MAX}（0.01 ppm）",
    )
    calib.set_defaults(handler=_cmd_set_calib, validate=_validate_set_calib)

    verify = sub.add_parser("verify", help="完整流程：对时→写种子→读回→屏上码比对→状态")
    _add_seed_arguments(verify, verify=True)
    verify.set_defaults(handler=_cmd_verify, validate=_validate_write_seed)

    return parser


# ---- 串口获取与入口 ----


def _candidate_ports() -> list[str]:
    """自动探测候选：只认 USB 类串口，避免误试内置 ttyS*。"""
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    markers = ("ttyusb", "ttyacm", "usbmodem", "usbserial", "wchusbserial")
    candidates: list[str] = []
    for info in list_ports.comports():
        name = info.device.lower()
        hwid = (info.hwid or "").lower()
        if "usb" in hwid or any(marker in name for marker in markers):
            candidates.append(info.device)
    return candidates


def _make_serial_device(port: str, trace: Callable[[str], None] | None) -> Device:
    return Device(session_mod.SerialSession(session_mod.open_serial(port), trace=trace))


def _open_device(
    args: argparse.Namespace,
    make_device: Callable[[str, Callable[[str], None] | None], Device],
    trace: Callable[[str], None] | None,
) -> Device:
    port = args.port or os.environ.get("TOTP_PORT")
    if port:
        return make_device(port, trace)

    candidates = _candidate_ports()
    if not candidates:
        raise ProvisionError("未发现 USB 串口：请用 --port 指定设备，或设置 TOTP_PORT 环境变量")

    failures: list[str] = []
    for candidate in candidates:
        try:
            device = make_device(candidate, trace)
        except ProvisionError as exc:
            failures.append(f"{candidate}（{exc}）")
            continue
        try:
            device.open(quick=True)
        except ProvisionError:
            failures.append(f"{candidate}（无响应）")
            device.close()
            continue
        print(f"已自动选择串口：{candidate}", file=sys.stderr)
        return device
    raise ProvisionError("自动探测失败：" + "；".join(failures) + "（请用 --port 指定）")


def _make_trace(verbosity: int) -> Callable[[str], None] | None:
    if verbosity <= 0:
        return None

    def trace(message: str) -> None:
        print(f"[调试] {message}", file=sys.stderr)

    return trace


def main(
    argv: Sequence[str] | None = None,
    *,
    make_device: Callable[[str, Callable[[str], None] | None], Device] | None = None,
) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    validate = getattr(args, "validate", None)
    if validate is not None:
        try:
            validate(args)
        except ValueError as exc:
            parser.error(str(exc))

    trace = _make_trace(args.verbose)
    factory = make_device or _make_serial_device
    try:
        device = _open_device(args, factory, trace)
    except ProvisionError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1

    try:
        with device:
            return args.handler(device, args)
    except (ProvisionError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("已中断", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
