# 拉取并固定版本的外部依赖（CMSIS 器件头 + STM32L0 HAL 驱动）。
#
# NOTE: 依赖不进仓库（见 .gitignore 的 vendor 规则），首次配置时下载到 firmware/vendor/。
#       版本号固定在下方，升级后需重新回归。
cmake_minimum_required(VERSION 3.20)

set(VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../vendor")

set(CMSIS_DEVICE_L0_REPO "https://github.com/STMicroelectronics/cmsis-device-l0.git")
set(CMSIS_DEVICE_L0_TAG  "v1.9.4")

set(STM32L0_HAL_REPO "https://github.com/STMicroelectronics/stm32l0xx-hal-driver.git")
set(STM32L0_HAL_TAG  "v1.10.7")

# CMSIS Core 只取 CMSIS_5 的 CMSIS/Core/Include 子目录：
# 用 --filter=blob:none --sparse 避免下载整个仓库（其中绝大部分是 M0+ 用不到的器件支持）。
# 该子目录没有独立标签，因此锁定到 commit。
set(CMSIS5_CORE_REPO "https://github.com/ARM-software/CMSIS_5.git")
set(CMSIS5_CORE_COMMIT "55b19837f5703e418ca37894d5745b1dc05e4c91")

function(totp_fetch_dep name repo tag)
  set(dest "${VENDOR_DIR}/${name}")
  if(NOT EXISTS "${dest}/.git")
    message(STATUS "拉取 ${name} @ ${tag}")
    execute_process(
      COMMAND git clone --depth 1 --branch ${tag} ${repo} ${dest}
      RESULT_VARIABLE rc
      OUTPUT_QUIET
    )
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "拉取 ${name} 失败；请检查网络，或手动 clone 到 ${dest}")
    endif()
  else()
    message(STATUS "${name} 已存在，跳过拉取")
  endif()
endfunction()

function(totp_fetch_cmsis_core)
  set(tmp "${VENDOR_DIR}/.cmsis5-tmp")
  set(dest "${VENDOR_DIR}/cmsis-core")
  if(EXISTS "${dest}/Include/core_cm0plus.h")
    message(STATUS "cmsis-core 已存在，跳过拉取")
    return()
  endif()

  message(STATUS "拉取 CMSIS Core @ ${CMSIS5_CORE_COMMIT}")
  file(REMOVE_RECURSE "${tmp}")
  execute_process(
    COMMAND git clone --filter=blob:none --sparse --depth 1 ${CMSIS5_CORE_REPO} ${tmp}
    RESULT_VARIABLE rc OUTPUT_QUIET
  )
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "拉取 CMSIS_5 失败；请检查网络，或手动准备 ${dest}/Include")
  endif()

  execute_process(
    COMMAND git -C ${tmp} fetch --depth 1 origin ${CMSIS5_CORE_COMMIT}
    RESULT_VARIABLE rc OUTPUT_QUIET
  )
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "拉取 CMSIS_5 指定 commit 失败：${CMSIS5_CORE_COMMIT}")
  endif()

  execute_process(
    COMMAND git -C ${tmp} sparse-checkout set CMSIS/Core/Include
    RESULT_VARIABLE rc OUTPUT_QUIET
  )
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "设置 CMSIS_5 sparse-checkout 失败")
  endif()

  execute_process(
    COMMAND git -C ${tmp} checkout --detach ${CMSIS5_CORE_COMMIT}
    RESULT_VARIABLE rc OUTPUT_QUIET
  )
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "检出 CMSIS_5 commit 失败：${CMSIS5_CORE_COMMIT}")
  endif()

  file(MAKE_DIRECTORY "${dest}")
  file(RENAME "${tmp}/CMSIS/Core/Include" "${dest}/Include")
  file(REMOVE_RECURSE "${tmp}")
endfunction()

totp_fetch_dep(cmsis-device-l0 ${CMSIS_DEVICE_L0_REPO} ${CMSIS_DEVICE_L0_TAG})
totp_fetch_dep(stm32l0xx-hal-driver ${STM32L0_HAL_REPO} ${STM32L0_HAL_TAG})
totp_fetch_cmsis_core()
