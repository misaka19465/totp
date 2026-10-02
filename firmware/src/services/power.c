/**
 * @file    power.c
 * @brief   低功耗管理实现（寄存器直写，接口见 power.h）。
 */
#include "power.h"

#include "stm32l031xx.h"

void power_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    /* NOTE: 复位后 PWR_CR 的 PDDS/PVDE/LPSDSR 均为 0；这里显式写死，
     *       避免后续改动误开 PVD 或 Standby。 */
    PWR->CR |= PWR_CR_LPSDSR;
    PWR->CR &= ~(PWR_CR_PDDS | PWR_CR_PVDE);
}

void power_enter_stop(void)
{
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
    /* NOTE: 唤醒后 MSI 重新起振由硬件处理；此处 ISB 只保证后续指令
     *       在唤醒后的时钟下取指。 */
    __ISB();
}
