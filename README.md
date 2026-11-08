# 打卡点动态码牌

这是一台离线 TOTP（Time-Based One-Time Password，基于时间的一次性口令）码牌。
设备放在活动点位上，段码屏常亮显示动态口令。
参与者到达点位后记录口令，活动组织者据此确认参与者到过该点位。
设备由一颗 CR2032 纽扣电池供电，口令在本地计算，无需联网。

> **用途边界**：这是活动道具，不是安全设备，不承担认证与防伪职责。
> 配置串口没有鉴权，种子以明文写入片内 EEPROM（可擦写数据存储器）。
> 任何接触设备的人都能读取或改写配置，请勿把它当作真实账号的第二因素。

## 功能

- 显示 1–6 位 TOTP 口令，默认 6 位。位数不足 6 位时右对齐，前导零照常显示。
- 32.768 kHz 晶体与 RTC（Real-Time Clock，实时时钟）负责走时。
- 空闲时进入低功耗的 Stop 模式，口令只在窗口边界重算。
- 每 10 分钟采样一次电量，用内部基准反推电池电压。
- 屏幕有 4 个状态小数点，含义见下表。
- 未写种子、时间不可信或故障时，数字位全部熄灭，避免参与者抄到无效码。

### 状态指示

| 点位 | 状态 | 说明 |
|------|------|------|
| DOT1 | 未写种子 | 设备还没有点位配置 |
| DOT2 | 需要重新对时 | 从未对时，或距上次对时超过 30 天 |
| DOT4 | 低电量 | 低电时常亮，极低电时以 1 Hz 闪烁 |
| DOT5 | 故障 | 自检失败，或发生过看门狗复位 |

看门狗复位后 DOT5 保持点亮，直到重新对时或写入种子。

## 硬件

| 部件 | 型号 | 说明 |
|------|------|------|
| 主控 | STM32L031F6P6 | Cortex-M0+，32 KB Flash（程序存储）/ 8 KB RAM（运行内存）/ 1 KB EEPROM（数据存储） |
| 显示 | HT1621B + EDS826S(T)P15V3B | 六位段码屏，含 4 个状态小数点 |
| 走时 | EPSON FC-135 | 32.768 kHz 音叉晶体 |
| 供电 | CR2032 | 无稳压器，无电源开关 |
| 配置口 | PJ-342 | 3.5 mm 耳机座，TRS 与 TRRS 插头都能用 |
| 烧录口 | 1×4 排针 | SWD（Serial Wire Debug，串行调试接口），仅用于烧录 |

一颗 CR2032 的续航约 2–4 个月（设计估算，屏驱动电流尚未实测）。
板上没有给 RTC 单独供电的引脚，也没有备份电容。
因此换电池必然丢失时间，换完需要重新对时。

## 仓库结构

```
docs/               设计与协议文档
firmware/           固件（C，交叉编译到 STM32L031）
  src/app/          口令计算、种子存储、串口命令
  src/drivers/      段码屏、串口、EEPROM、电量等驱动
  src/services/     走时、电源、看门狗、状态指示
  tests/            主机侧单元测试
```

上位机配置工具单独维护在同级 `../provision/` 项目中，两个固件共用同一份协议实现。

`firmware/vendor/` 存放首次构建时自动拉取的三方依赖，不进入版本库。

## 构建与烧录

需要 `arm-none-eabi-gcc` 与 CMake 3.20 或更高版本。
首次配置时，CMake 用 git 拉取锁定版本的 CMSIS 与 HAL 源码，因此需要网络。

```bash
cd firmware
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cfg
cmake --build build
```

产物为 `firmware/build/totp_token.elf`、`totp_token.bin` 与 `totp_token.hex`。

烧录需要 OpenOCD 和调试器，默认按 ST-Link 配置：

```bash
cmake --build build --target flash
```

使用 CMSIS-DAP 时，在配置阶段加上 `-DTOTP_OPENOCD_INTERFACE=interface/cmsis-dap.cfg`。

## 测试

固件测试只编译纯计算模块，不需要硬件与交叉工具链：

```bash
cmake -S firmware/tests -B firmware/tests/build
cmake --build firmware/tests/build
ctest --test-dir firmware/tests/build --output-on-failure
```

用例取自 RFC（Request for Comments，互联网标准文档）的官方测试向量，
包括 RFC 4648、RFC 2202、RFC 4226 与 RFC 6238。
覆盖 Base32 解码、HMAC-SHA1（基于 SHA-1 的消息认证码）、TOTP 算法、
种子存储、串口协议与显示渲染。

配置工具测试：

```bash
python3 -m unittest discover -s ../provision -v
```

## 配置工具

工具依赖 `pyserial`：

```bash
python3 -m venv ../provision/.venv
../provision/.venv/bin/pip install -r ../provision/requirements.txt
```

活动前运行一条命令即可完成配置与验收：

```bash
../provision/.venv/bin/python ../provision/provision.py setup
../provision/.venv/bin/python ../provision/provision.py setup JBSWY3DPEHPK3PXP
```

`setup` 执行完整流程：写种子 → READ_META 字段与 CRC 回读比对 → 对时命令回读 → 读取状态。
配置结果由设备命令响应核验，不需要人工听码或对屏幕读数。
省略种子时工具用系统安全随机源生成 20 字节（160 bit）种子，写入前打印一次，
需立即登记到服务器；写入后工具不再回显。

需要单独操作时，也可以分步执行：

```bash
../provision/.venv/bin/python ../provision/provision.py ping
../provision/.venv/bin/python ../provision/provision.py set-time
../provision/.venv/bin/python ../provision/provision.py write-seed --digits 6 --period 30
../provision/.venv/bin/python ../provision/provision.py write-seed JBSWY3DPEHPK3PXP
```

子命令一览：

| 命令 | 用途 |
|------|------|
| `ping` | 握手，核对协议与固件版本 |
| `get-time` | 读取设备 UTC 时间与可信标志 |
| `set-time` | 用电脑时间对时，回读误差 ≤100 ms |
| `status` | 读取电量、对时、复位原因与故障 |
| `read-meta` | 读取配置元信息，不回显种子 |
| `write-seed` | 写入种子并读回比对（不给种子则随机生成） |
| `erase` | 删除种子 |
| `set-calib` | 写入 RTC 校准值（0.01 ppm，0 表示关闭） |
| `setup` | 完整配置与验收流程（不给种子则随机生成） |

- `--digits` 取 1–6，`--period` 取 15–300 秒（与 UV-K5 共用默认值 60 秒），`--random-bytes` 取 1–35（默认 20）。
- `--port` 缺省时自动探测 USB 串口，也可以用环境变量 `TOTP_PORT` 指定。

### 接线

配置工具通过 USB-TTL 适配器连接设备的耳机口：

| 耳机插头 | 设备信号 | USB-TTL 适配器 |
|----------|----------|----------------|
| TIP | 设备发送 | RXD |
| RING1 | 设备接收 | TXD |
| SLEEVE | 地 | GND |

设备引脚为 5 V 容限，可以直接连接 5 V 与 3.3 V 的适配器。
配置完成后拔下插头即可，设备会自动回到休眠。

## 串口协议

- 物理层：38400 8N1。
- 帧格式：帧头 `0xAA`，CRC-16（16 位循环冗余校验），载荷上限 64 B。
- 设备只回响应，不主动发送；所有多字节字段小端。
- 设备空闲时休眠；接收线出现下降沿即唤醒；会话闲置 10 秒后自动回睡。
- 协议没有鉴权与加密，只防误码，不防攻击。

完整规范见 `docs/serial-protocol.md`。

使用新配置工具前，已有 TOTP 设备需要重新烧录本版固件以采用统一的 38400 速率。

## 项目状态

- 固件与配置工具已完成端到端流程，并在首板上验证通过。
- 主机侧测试覆盖算法、存储、协议与显示渲染，全部通过。
- 待实机标定：电量阈值与整机功耗。
