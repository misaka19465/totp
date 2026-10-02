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
tools/provision/ 上位机配置工具（待实现）
```

## 文档

| 文档 | 内容 |
|------|------|
| `docs/requirements.md` | 需求规格（产品定位、功能/非功能需求、状态指示、对时方式） |
| `docs/architecture.md` | 软件分层、低功耗策略、主控选型结论 |
| `docs/hardware.md` | 硬件审定：引脚、段码映射、功耗分析、已发现的硬件问题 |
| `docs/serial-protocol.md` | 耳机口串口协议 v1.1（帧格式、对时、写种子、插头检测） |

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

烧录（需 OpenOCD 与 CMSIS-DAP 调试器）：

```bash
cmake --build build --target flash
```

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

initial commit —— 已完成项目骨架、构建系统（已验证可编译出可运行固件）与设计文档；
功能代码尚未实现。落地顺序见 `docs/architecture.md` §10。
