# 打卡点动态码牌（TOTP）

基于 STM32L031F6P6 + HT1621B + EDS826 六位段码屏的**离线**打卡码牌：放在活动的
指定点位上，屏常亮显示随时间变化的数字码（支持 1..6 位，本场为 6 位；不足 6 位时
右对齐、高位留空）；参与者到达点位后记录该码，活动方据此确认"本人到过该点位"。
CR2032 纽扣电池供电，码在本地计算，无需联网。

> **用途边界**：这是活动道具，不是安全密钥，不承担认证/防伪职责。
> 因此不做 RDP、串口鉴权、种子加密等安全设计；设计目标只有两个：
> 现场简单够用、一颗电池撑住活动周期（功耗分析见 `docs/hardware.md` H1）。

- **主控**：STM32L031F6P6（Cortex-M0+，32 KB Flash / 8 KB RAM / 1 KB 数据 EEPROM）
- **显示**：HT1621B 驱动 EDS826S(T)P15V3B 六位段码屏，4 个状态小数点
- **走时**：32.768 kHz 音叉晶体 + RTC
- **对时 / 写种子**：3.5 mm 耳机口（UART），活动前由组织者配置（插上即信任）
- **固件烧录**：SWD（4 针排针），**不承载产品功能**

## 仓库结构

```
docs/            设计与需求文档（先读这里）
firmware/        固件源码与构建系统
tools/provision/ 上位机配置工具（命令行，python3 + pyserial）
```

## 文档

| 文档 | 内容 |
|------|------|
| `docs/requirements.md` | 需求规格（产品定位、功能/非功能需求、状态指示、对时方式） |
| `docs/architecture.md` | 软件分层、低功耗策略、主控选型结论 |
| `docs/hardware.md` | 硬件审定：引脚、段码映射、功耗分析、已发现的硬件问题 |
| `docs/serial-protocol.md` | 耳机口串口协议 v1.2（帧格式、对时、写种子、插头检测） |

> **注意**：`docs/requirements.md` §1–§3 记录了已确认的关键取舍（无按键、常亮、
> 2–4 个月电池寿命、非安全设备），建议先读；全部设计决策已关闭，
> 剩余的实机标定与试验项见 `docs/architecture.md` §11。

## 构建

依赖 `arm-none-eabi-gcc` 与 CMake ≥ 3.20。三方依赖（CMSIS / HAL）在首次配置时自动拉取，
版本锁定见 `firmware/vendor/VERSIONS.md`。

```bash
cd firmware
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cfg
cmake --build build
```

产物：`firmware/build/totp_token.{elf,bin,hex}`。

烧录（需 OpenOCD 与调试器；默认按 ST-Link 配置，用 CMSIS-DAP 时以
`-DTOTP_OPENOCD_INTERFACE=interface/cmsis-dap.cfg` 重新配置）：

```bash
cmake --build build --target flash
```

### 主机侧单元测试

`firmware/tests/` 用 RFC 4648 / 2202 / 4226 / 6238 官方向量校验纯计算模块
（Base32 解码、HMAC-SHA1、HOTP/TOTP），不依赖硬件与交叉工具链：

```bash
cmake -S firmware/tests -B firmware/tests/build
cmake --build firmware/tests/build
ctest --test-dir firmware/tests/build --output-on-failure
```

## 配置工具（活动前）

`tools/provision/provision.py` 是组织者用的命令行配置工具，实现
`docs/serial-protocol.md` §6 的完整流程，依赖 `pyserial`：

```bash
python3 -m pip install pyserial

./tools/provision/provision.py -p /dev/ttyUSB0 ping          # 握手与版本检查
./tools/provision/provision.py -p /dev/ttyUSB0 set-time      # 用主机 UTC 对时
./tools/provision/provision.py -p /dev/ttyUSB0 write-seed JBSWY3DPEHPK3PXP
./tools/provision/provision.py -p /dev/ttyUSB0 verify JBSWY3DPEHPK3PXP
```

- `verify` 执行 §6 全流程：对时 → 写种子 → READ_META 读回比对 → 显示当前
  口令与剩余秒数供操作员与屏上数字比对 → GET_STATUS；`--yes` 跳过人工比对。
- 其余子命令：`get-time`、`status`、`read-meta`、`erase`、`set-calib`。
- `--port` 缺省时自动探测 USB 串口，也可用 `TOTP_PORT` 环境变量指定。
- 主机侧测试：`python3 -m unittest discover -s tools/provision -v`
  （§8 逐字节向量 + 会话重传/恢复 + TOTP RFC 向量 + 进程内设备仿真的 CLI 全流程）。

## 硬件注意事项

以下三条是从原理图与器件手册中查出、且**会影响设计决策**的问题，
详见 `docs/hardware.md`：

1. **常亮功耗**：屏驱动约 60–120 µA，CR2032 只能支撑 **2–4 个月**。
   板上没有给 HT1621B 断电的器件，软件无法规避。
2. **换电池必丢时间**：TSSOP-20 封装无 VBAT 引脚，板上也无备份电容。
3. **第 4~6 位的显示 RAM 地址不连续**：屏 pin 11..16 接 SEG13..SEG18，
   因此第 4~6 位落在地址 `0x0D~0x12`（前三位在 `0x00~0x05`）。
   六位数字本身都是完整 7 段，段码字形无缺段问题。

## 状态

- 已完成项目骨架、构建系统（已验证可编译出可运行固件）与设计文档；
- 已完成 App 层口令计算核心 `app/base32`、`app/hmac_sha1`、`app/totp`，
  含主机侧 RFC 向量测试；
- 已完成驱动层 `drivers/ht1621`（三线串口）、`drivers/lcd_render`（字形表、
  1..6 位右对齐、4 个状态点）、`drivers/battery`（VREFINT 反推 VDD）与耳机口
  链路层 `drivers/uart_link` + `drivers/plug_detect` + `drivers/usart2_hw`
  （帧编解码、CRC、重同步、唤醒/会话超时、EXTI 唤醒），主机侧测试合计
  340 项断言，全绿；
- 已完成应用层 `app/keystore`（数据 EEPROM 单槽配置记录：magic/版本/独立
  记录 CRC、参数校验、幂等写入、写后读回比对与失败回滚；记录同时保存
  Base32 字符数以支持 READ_META 回显，记录版本升为 v2，旧 v1 记录
  按未写入处理（需重写种子），配套
  `drivers/nvm`（EEPROM 字级擦写）与公共 `common/crc16`；
- 已完成服务层 `services/clock_rtc`（LSE 启振、RTC 1 Hz + 1/256 s 亚秒、
  UTC 毫秒读写、`RTC_SHIFTR` 亚秒对齐、备份寄存器记录对时状态与
  TRUSTED/NEVER_SET/STALE 标志、CALR 数字校准折算），纯换算部分
  `services/rtc_time` 配有主机侧测试（含 2000..2099 全时段逐日往返）；
  主机侧测试合计 5829 项断言，全绿；`main.c` 开机接入 RTC 初始化与
  DOT5 故障告警；
- 已完成应用层 `app/proto`（serial-protocol.md §5 全部 8 条命令的分派、
  字段校验与响应组帧：PING/GET_TIME/SET_TIME/GET_STATUS/WRITE_SLOT/
  READ_META/ERASE_SLOT/SET_CALIB），与 keystore/clock_rtc 对接；
  主机侧测试用 §8 逐字节向量 + EEPROM 失败注入覆盖；
- 已完成服务层 `services/power`（Stop 档位与进出）、`services/watchdog`
  （IWDG 4 s、复位原因归并、BKP2R 故障锁存）与 `services/state_indicator`
  （状态点掩码、极低电量 1 Hz 闪烁、故障/未对时清空数字位）；
- 已完成主循环接线（`main.c`）：空闲时按"口令窗口边界 / 显示重写（2 s，
  极低电量 1 s）/ 电量采集（10 min）"取最早期限武装 RTC 闹钟进 Stop，
  PA10 EXTI 唤醒插头会话；会话期间保持清醒，口令只在窗口边界重算，
  对时/写种子成功后立即刷新显示并清除故障锁存；RTC 不可用时降级为
  仅 EXTI 唤醒并锁存故障（不进入 IWDG 复位循环）；
  主机侧测试合计 5982 项断言，全绿；
- 已完成上位机配置工具 `tools/provision`（`ping`/`get-time`/`set-time`/`status`/
  `read-meta`/`write-seed`/`erase`/`set-calib`/`verify`）：会话层按 §7 实现
  前导码、重传与会话恢复，命令层做写后读回与 `config_crc16` 复算，`verify`
  按 §6 跑完整流程并显示口令供屏上比对；主机侧测试 69 项，全绿。
- 剩余：实机标定（`docs/architecture.md` §11 清单）。
- 实机调试（2026-10-02，首板）：修复两处"外设时钟未就绪即等待"的死锁
  （ADC 校准前未切 `CKMODE`；IWDG 先配置后使能导致 LSI 未开时卡在 SR 轮询）；
  LSE 驱动档按实测改为中高（低档冷启动 6 s 超时、中高档 ~300 ms）；
  修复 RTC 闹钟在 EXTI 线 17 上缺少上升沿使能（`RTSR`）导致空闲约 4 s 被
  IWDG 反复复位的问题；电池采样实读 3.35 V、RTC 走时与 IWDG 均正常；
  串口配置流程已接通实物验证
  （ping/对时/读时间/状态/写种子/读回/擦除/校准/verify 全通过，写入
  `JBSWY3DPEHPK3PXP` 后 `config_crc16=0xB638`，与规范 §8 向量一致；对时回读
  偏差 +10~15 ms，上位机容差 ±100 ms，见 serial-protocol.md §5.4）。
