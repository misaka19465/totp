# 外部依赖版本锁定

本目录下的第三方源码**不进入版本库**（见仓库根 `.gitignore`），
首次配置时由 `firmware/cmake/fetch_vendor.cmake` 按下方标签拉取。

| 目录 | 上游仓库 | 锁定标签 | 用途 | 许可证 |
|------|----------|----------|------|--------|
| `cmsis-device-l0/` | STMicroelectronics/cmsis-device-l0 | `v1.9.4` | STM32L0 器件头与 `system_stm32l0xx.c` | Apache-2.0 |
| `cmsis-core/` | ARM-software/CMSIS_5（`CMSIS/Core/Include`） | commit `55b19837f5703e418ca37894d5745b1dc05e4c91` | Cortex-M0+ 内核头（`core_cm0plus.h` 等） | Apache-2.0 |
| `stm32l0xx-hal-driver/` | STMicroelectronics/stm32l0xx-hal-driver | `v1.10.7` | HAL 公共部分（时钟/电源/复位） | BSD-3-Clause |

## 升级流程

1. 修改 `firmware/cmake/fetch_vendor.cmake` 中的标签，或同步更新本表。
2. 删除对应 `vendor/<目录>` 后重新执行 CMake 配置以重新拉取。
3. 跑一次完整构建并核对 `firmware/build/totp_token.map` 中的体积变化。
4. 提交时把本表的版本号一并更新。

## 为什么 CMSIS Core 用 commit 而不是标签

`cmsis-core/` 只取 `CMSIS_5` 仓库的 `CMSIS/Core/Include` 子目录，
用 `--filter=blob:none --sparse` 拉取以免下载整个 CMSIS_5（含大量与 M0+ 无关的器件支持）。
目前没有覆盖该子目录的稳定标签，故锁定到具体 commit。
