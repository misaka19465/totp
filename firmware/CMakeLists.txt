cmake_minimum_required(VERSION 3.20)

project(totp_token
        VERSION 0.1.0
        DESCRIPTION "CR2032 供电的打卡点动态码牌（离线 TOTP 段码）"
        LANGUAGES C ASM)

# NOTE: 工具链固定为 arm-none-eabi，不接受宿主默认编译器，
#       避免在 CI 上误用宿主机 cc 产出无法烧录的目标文件。
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# NOTE: 这里必须写字面量而不是展开 ${MCU_FLAGS}：MCU_FLAGS 是列表，
#       在双引号内展开会得到以分号分隔的单个参数，汇编器无法解析。
set(CMAKE_ASM_FLAGS "-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -x assembler-with-cpp -O3 -DNDEBUG")

option(TOTP_WERROR     "把本工程代码的编译器告警视为错误" ON)
option(TOTP_ENABLE_LTO "启用链接时优化以压缩体积"          OFF)

set(CMAKE_C_FLAGS_DEBUG   "-Og -g3 -gdwarf-4")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0 -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections")

# NOTE: 严格的告警策略只作用于本工程代码（totp_token.elf）。
#       厂商代码（CMSIS/HAL）保持其上游语义，不参与本项目的告警契约，
#       否则每次升级厂商包都要重新适配一遍无关告警。
set(TOTP_STRICT_WARNINGS -Wall -Wextra -Wshadow -Wundef -Wdouble-promotion -Wswitch-enum)

# 首次配置时自动拉取锁定版本的三方依赖（版本表见 vendor/VERSIONS.md）
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch_vendor.cmake)

set(VENDOR_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(CMSIS_DEVICE_DIR  ${VENDOR_DIR}/cmsis-device-l0)
set(CMSIS_CORE_DIR    ${VENDOR_DIR}/cmsis-core)
set(HAL_DIR           ${VENDOR_DIR}/stm32l0xx-hal-driver)

# ---- 厂商依赖 ----
add_library(vendor_cmsis STATIC ${CMSIS_DEVICE_DIR}/Source/Templates/system_stm32l0xx.c)
target_compile_options(vendor_cmsis PRIVATE ${MCU_FLAGS})
target_compile_definitions(vendor_cmsis PUBLIC STM32L031xx)
target_include_directories(vendor_cmsis SYSTEM PUBLIC
  ${CMSIS_DEVICE_DIR}/Include
  ${CMSIS_CORE_DIR}/Include
)

# NOTE: 当前应用层尚未调用 HAL 任何符号（外设均为寄存器直写），
#       保留该目标是为了让 HAL 源码处于可编译验证状态，
#       一旦确认不再需要即可整体删除并同步删除 src/hal/。
add_library(hal_l0 STATIC ${HAL_DIR}/Src/stm32l0xx_hal.c)
target_compile_options(hal_l0 PRIVATE ${MCU_FLAGS})
target_include_directories(hal_l0 SYSTEM PUBLIC ${HAL_DIR}/Inc)
target_include_directories(hal_l0 SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hal)
target_link_libraries(hal_l0 PUBLIC vendor_cmsis)

# ---- 应用固件 ----
add_executable(totp_token.elf
  startup/startup_stm32l031.s
  src/main.c
  src/app/base32.c
  src/app/hmac_sha1.c
  src/app/totp.c
  src/drivers/battery.c
  src/drivers/ht1621.c
)

target_compile_options(totp_token.elf PRIVATE
  ${MCU_FLAGS}
  ${TOTP_STRICT_WARNINGS}
  -fno-common -fno-strict-aliasing -ffreestanding
  $<$<BOOL:${TOTP_WERROR}>:-Werror>
)

target_include_directories(totp_token.elf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board
  ${CMAKE_CURRENT_SOURCE_DIR}/src/drivers
  ${CMAKE_CURRENT_SOURCE_DIR}/src/hal
)

target_link_libraries(totp_token.elf PRIVATE vendor_cmsis)
target_link_options(totp_token.elf PRIVATE
  ${MCU_FLAGS}
  -T${CMAKE_CURRENT_SOURCE_DIR}/linker/stm32l031.ld
  -nostartfiles
  -Wl,-Map=${CMAKE_BINARY_DIR}/totp_token.map,--cref
  -lc -lnosys -lm
)
set_target_properties(totp_token.elf PROPERTIES SUFFIX "")

add_custom_command(TARGET totp_token.elf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:totp_token.elf> ${CMAKE_BINARY_DIR}/totp_token.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:totp_token.elf> ${CMAKE_BINARY_DIR}/totp_token.hex
  COMMAND ${CMAKE_SIZE} -A -x $<TARGET_FILE:totp_token.elf>
  COMMENT "生成 bin/hex 并报告体积"
)

add_custom_target(flash
  COMMAND openocd -f interface/cmsis-dap.cfg -f target/stm32l0.cfg
          -c "program ${CMAKE_BINARY_DIR}/totp_token.elf verify reset exit"
  DEPENDS totp_token.elf
  COMMENT "通过 SWD (J2) 烧录并校验"
)
