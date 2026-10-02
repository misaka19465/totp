/**
 * @file    watchdog.c
 * @brief   IWDG 看门狗与故障锁存实现（寄存器直写，接口见 watchdog.h）。
 */
#include "watchdog.h"

#include "stm32l031xx.h"

/* IWDG 访问键（RM0367 §23.4.4） */
#define WATCHDOG_KEY_ACCESS  0x5555U
#define WATCHDOG_KEY_RELOAD  0xAAAAU
#define WATCHDOG_KEY_ENABLE  0xCCCCU

/* NOTE: LSI 标称 37 kHz，手册全温区 26..56 kHz，因此实际超时约 2.6..5.7 s：
 *       取分频 256（PR=6）、重装 578 → 标称 4.0 s。主循环空闲期最长一次
 *       Stop ≤ 2 s（显示重写间隔），最坏情况下仍留有余量。 */
#define WATCHDOG_PRESCALER   (6U)     /* /256 */
#define WATCHDOG_RELOAD      (578U)   /* 4.0 s @ 37 kHz */

/* BKP2R 锁存 magic；与 clock_rtc 的 BKP0R/BKP1R 约定不冲突 */
#define WATCHDOG_BKP_LATCH_MAGIC 0x4641554CU /* 'FAUL' */

static bool s_started;
static bool s_fault_latched;
static watchdog_reset_cause_t s_reset_cause = WATCHDOG_RESET_UNKNOWN;

void watchdog_init(void)
{
    const uint32_t csr = RCC->CSR;
    s_reset_cause = watchdog_classify_reset(
        (csr & RCC_CSR_IWDGRSTF) != 0U,
        (csr & RCC_CSR_SFTRSTF) != 0U,
        (csr & (RCC_CSR_PORRSTF | RCC_CSR_LPWRRSTF)) != 0U,
        (csr & (RCC_CSR_PINRSTF | RCC_CSR_OBLRSTF | RCC_CSR_FWRSTF | RCC_CSR_WWDGRSTF)) != 0U);

    s_fault_latched = (RTC->BKP2R == WATCHDOG_BKP_LATCH_MAGIC) ||
                      (s_reset_cause == WATCHDOG_RESET_IWDG);
    if (s_reset_cause == WATCHDOG_RESET_IWDG) {
        RTC->BKP2R = WATCHDOG_BKP_LATCH_MAGIC;
    }

    /* 清复位标志，使下一次复位的原因可判定 */
    RCC->CSR |= RCC_CSR_RMVF;
}

void watchdog_start(void)
{
    if (s_started) {
        return;
    }
    /* NOTE: 必须先写 0xCCCC 使能：只有使能时 IWDG 才强制打开 LSI，而 PR/RLR
     *       的写同步（SR.PVU/RVU 清零）发生在 LSI 时钟域。先配置再使能会永久
     *       卡在 SR 轮询（实机复现：PC 停在 SR 等待、RCC_CSR.LSION=0）。
     *       顺序与 ST HAL_IWDG_Init 一致。 */
    IWDG->KR = WATCHDOG_KEY_ENABLE;
    IWDG->KR = WATCHDOG_KEY_ACCESS;
    IWDG->PR = WATCHDOG_PRESCALER;
    IWDG->RLR = WATCHDOG_RELOAD;
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U) {
        /* LSI 已在跑：同步只需几个 LSI 周期（~100 µs） */
    }
    IWDG->KR = WATCHDOG_KEY_RELOAD;
    s_started = true;
}

void watchdog_feed(void)
{
    if (s_started) {
        IWDG->KR = WATCHDOG_KEY_RELOAD;
    }
}

bool watchdog_enabled(void)
{
    return s_started;
}

watchdog_reset_cause_t watchdog_reset_cause(void)
{
    return s_reset_cause;
}

bool watchdog_fault_latched(void)
{
    return s_fault_latched;
}

void watchdog_latch_fault(void)
{
    s_fault_latched = true;
    RTC->BKP2R = WATCHDOG_BKP_LATCH_MAGIC;
}

void watchdog_clear_fault(void)
{
    s_fault_latched = false;
    RTC->BKP2R = 0U;
}
