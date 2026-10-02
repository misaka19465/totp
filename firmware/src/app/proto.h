/**
 * @file    proto.h
 * @brief   耳机口协议命令层：命令分派、字段校验与响应组帧。
 *
 * 规范：docs/serial-protocol.md §5。链路层（drivers/uart_link）负责收帧、
 * CRC 与重同步，本层只处理 CRC 合法、LEN ≤ 64 的请求帧，逐条命令完成
 * 字段校验 → 对接 keystore / clock_rtc / 状态快照 → 回一条响应。
 * 同一时刻只允许一个未决请求（§4.5），因此本层无并发状态。
 *
 * NOTE: 电量、复位原因、运行时长与故障锁存属于电源/看门狗职责（尚未落地），
 *       由主循环填进 proto_status_t 传入；本层不直接访问寄存器/外设，
 *       种子与时间通过 keystore / clock_rtc 服务读写，主机侧测试可完整覆盖。
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "uart_link.h"

/** 协议版本（PING.proto_ver，serial-protocol.md §5.2） */
#define PROTO_VERSION      0x01U
/** 固件版本 major<<8 | minor；1.0，与 §8 的 PING 响应向量一致 */
#define PROTO_FW_VERSION   0x0100U
/** 本设备允许的最大 PAYLOAD（PING.max_payload，§5.2） */
#define PROTO_MAX_PAYLOAD  UART_LINK_MAX_PAYLOAD

/** SET_CALIB 请求的 0.01 ppm 单位范围（§5.8） */
#define PROTO_CAL_CENTI_PPM_MAX 30000

/** 命令码（§5.1）；0x80..0xFF 仅作响应码，设备收到时回 ERR_UNKNOWN_CMD */
typedef enum {
    PROTO_CMD_PING       = 0x01,
    PROTO_CMD_GET_TIME   = 0x02,
    PROTO_CMD_SET_TIME   = 0x03,
    PROTO_CMD_GET_STATUS = 0x04,
    PROTO_CMD_WRITE_SLOT = 0x05,
    PROTO_CMD_READ_META  = 0x06,
    PROTO_CMD_ERASE_SLOT = 0x07,
    PROTO_CMD_SET_CALIB  = 0x08,
} proto_cmd_t;

/** 响应状态码（§5.9）；0x02 保留：CRC 错帧设备静默丢弃、从不发送 */
typedef enum {
    PROTO_OK              = 0x00,
    PROTO_ERR_LEN         = 0x01,
    PROTO_ERR_UNKNOWN_CMD = 0x03,
    PROTO_ERR_PARAM       = 0x04,
    PROTO_ERR_STATE       = 0x05, /**< 保留，暂不产生 */
    PROTO_ERR_EEPROM      = 0x06,
    PROTO_ERR_BUSY        = 0x07, /**< 保留，暂不产生 */
    PROTO_ERR_UNSUPPORTED = 0x08,
    PROTO_ERR_INTERNAL    = 0xFF,
} proto_result_t;

/** PING 与 GET_STATUS 共用的设备状态位（§5.10） */
#define PROTO_FLAG_SEED_PRESENT     (1U << 0)
#define PROTO_FLAG_RTC_NEVER_SET    (1U << 1)
#define PROTO_FLAG_RTC_STALE        (1U << 2)
#define PROTO_FLAG_LOW_BATTERY      (1U << 3)
#define PROTO_FLAG_CRITICAL_BATTERY (1U << 4)
#define PROTO_FLAG_FAULT_LATCHED    (1U << 5)
#define PROTO_FLAG_CALIB_ACTIVE     (1U << 6)
#define PROTO_FLAG_IWDG_ENABLED     (1U << 7)

/** READ_META.flags 位定义（§5.7） */
#define PROTO_META_FLAG_SEED_PRESENT (1U << 0)
#define PROTO_META_FLAG_RECORD_VALID (1U << 1)

/** GET_STATUS.reset_cause 取值（§5.5）；由主循环从复位原因寄存器归并 */
typedef enum {
    PROTO_RESET_POR      = 0, /**< POR/BOR */
    PROTO_RESET_IWDG     = 1,
    PROTO_RESET_SOFTWARE = 2,
    PROTO_RESET_OTHER    = 3,
    PROTO_RESET_UNKNOWN  = 4,
} proto_reset_cause_t;

/**
 * @brief  设备状态快照：PING/GET_STATUS 中非服务模块可提供的信息。
 *
 * NOTE: uptime_s、reset_cause、fault_latched、iwdg_enabled 在电源/看门狗
 *       服务落地后由主循环维护；当前阶段主循环可直接填常量。
 */
typedef struct {
    uint32_t        uptime_s;      /**< 上电以来的秒数（RTC 不可用时为近似值） */
    uint8_t         reset_cause;   /**< proto_reset_cause_t                   */
    uint16_t        battery_mv;    /**< 滤波后的 VDD 毫伏；未知为 0           */
    battery_level_t battery_level; /**< 电量档位                              */
    bool            fault_latched; /**< 故障锁存（含 IWDG 复位、自检失败）    */
    bool            iwdg_enabled;  /**< 看门狗已启用                          */
} proto_status_t;

/**
 * @brief  处理一帧请求：执行命令并回一条响应。
 * @param  request 已通过 CRC 校验、LEN ≤ 64 的请求帧。
 * @param  status  设备状态快照（不可为 NULL）。
 * @return 响应已交给链路返回 true；参数非法或链路已休眠返回 false。
 * @note   除发送失败外，任何请求都会得到响应（错误只含状态字节，§4.5）；
 *         响应延迟远低于 §7 的 200 ms 上限（存储命令最坏约 90 ms）。
 */
bool proto_handle_request(const uart_link_frame_t *request,
                          const proto_status_t *status);

#endif /* PROTO_H */
