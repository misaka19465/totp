/**
 * @file    battery.c
 * @brief   用内部 VREFINT 反推 VDD 的电量监测实现（寄存器直写）。
 *
 * 原理与限制见 battery.h。
 *
 * NOTE: ADC 时钟取 PCLK/2。本工程内核跑 MSI 2.097 MHz，PCLK/2 ≈ 1.05 MHz，
 *       低于 2.8 MHz，按手册必须置 LFMEN（低频率模式），否则转换结果不可信。
 * NOTE: 每次测量都重新上电 ADC 并做一次校准（数 µs 量级），测完立刻断电，
 *       避免 ADC 静态电流常驻 —— 本设备绝大部分时间在 Stop 模式。
 */
#include "battery.h"
#include "stm32l031xx.h"

/* 出厂校准值：25 °C、VDDA = 3.0 V 条件下 VREFINT 的 12 bit 原始码值 */
#define VREFINT_CAL_ADDR     ((const uint16_t *)0x1FF80078U)
#define VREFINT_CAL_VREF_MV  3000U

/* 出厂默认阈值；标定方法见 battery.h */
#define BATTERY_THRESHOLD_LOW_MV       2400U
#define BATTERY_THRESHOLD_CRITICAL_MV  2200U

static bool s_initialized = false;

/**
 * @brief  极粗略的忙等延时。
 * @note   仅用于 ADC 稳压器启动等"宁长勿短"的场合：
 *         MSI 2.097 MHz 下每次循环约 6 个周期 ≈ 3 µs。
 */
static void delay_loops(uint32_t loops)
{
    for (volatile uint32_t i = 0U; i < loops; i++) {
        __asm volatile ("nop");
    }
}

/** ADC 上电：使能外设时钟、打开内部稳压器并等待其稳定 */
static void adc_power_up(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    ADC1->CR |= ADC_CR_ADVREGEN;
    delay_loops(20U);   /* tADCVREG_STUP 最大 10 µs，此处约 60 µs，留足余量 */
}

/** ADC 断电：关稳压器、关外设时钟 */
static void adc_power_down(void)
{
    if ((ADC1->CR & ADC_CR_ADEN) != 0U) {
        ADC1->CR |= ADC_CR_ADDIS;
        while ((ADC1->CR & ADC_CR_ADEN) != 0U) {
            /* 等待 ADC 完全关闭 */
        }
    }
    ADC1->CR &= ~ADC_CR_ADVREGEN;
    RCC->APB2ENR &= ~RCC_APB2ENR_ADC1EN;
}

/** 触发一次通道 17（VREFINT）转换并返回原始码值 */
static uint16_t adc_convert_vrefint(void)
{
    /* NOTE: 校准需要 ADC 时钟在跑，而 CFGR2 复位值 CKMODE=00 选择 ADCCLK
     *       （片内 HSI16）。本工程常驻 MSI、HSI16 只在串口会话期间开启，
     *       因此必须先把时钟切到 PCLK/2 再启动校准，否则 ADCAL 永不归零
     *       （实机表现为 CM0+ 卡死在等待循环、被 IWDG 反复复位）。
     *       CKMODE 只能在 ADEN = 0 时修改，故整段配置放在使能 ADC 之前。 */
    ADC1->CFGR2 &= ~ADC_CFGR2_CKMODE;
    ADC1->CFGR2 |= ADC_CFGR2_CKMODE_0;      /* 同步时钟 PCLK/2 */
    /* 低频率模式：PCLK/2 ≈ 1.05 MHz < 2.8 MHz，见文件头 NOTE */
    ADC->CCR |= ADC_CCR_LFMEN | ADC_CCR_VREFEN;

    /* 12 bit（RES = 00）、右对齐、单次转换、软件触发 */
    ADC1->CFGR1 &= ~(ADC_CFGR1_CONT | ADC_CFGR1_RES | ADC_CFGR1_ALIGN);

    /* 最长采样时间（160.5 周期）：VREFINT 源阻抗高，需足够采样窗口 */
    ADC1->SMPR = ADC_SMPR_SMP;
    ADC1->CHSELR = ADC_CHSELR_CHSEL17;

    /* 校准必须在 ADEN = 0 时进行，且需 ADC 稳压器已上电（adc_power_up） */
    ADC1->CR |= ADC_CR_ADCAL;
    while ((ADC1->CR & ADC_CR_ADCAL) != 0U) {
        /* 等待校准完成（约 5 µs） */
    }

    ADC1->CR |= ADC_CR_ADEN;
    while ((ADC1->ISR & ADC_ISR_ADRDY) == 0U) {
        /* 等待 ADC 就绪 */
    }
    delay_loops(3U);   /* 采样开始前留出稳定时间 tSTAB（典型 1 µs） */

    ADC1->CR |= ADC_CR_ADSTART;
    while ((ADC1->ISR & ADC_ISR_EOC) == 0U) {
        /* 等待转换结束 */
    }

    return (uint16_t)(ADC1->DR & 0x0FFFU);
}

void battery_init(void)
{
    /* 只开 SYSCFG 时钟；VREFINT 缓冲器在每次测量时按需开关（见 read） */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    s_initialized = true;
}

uint32_t battery_read_mv(void)
{
    if (!s_initialized) {
        return 0U;
    }

    /* 打开 VREFINT 到 ADC 的缓冲器，等它就绪（典型 2~3 µs） */
    SYSCFG->CFGR3 |= SYSCFG_CFGR3_ENBUF_VREFINT_ADC;
    while ((SYSCFG->CFGR3 & SYSCFG_CFGR3_VREFINT_RDYF) == 0U) {
        /* 等待内部参考就绪 */
    }

    adc_power_up();
    uint16_t raw = adc_convert_vrefint();
    adc_power_down();

    /* 缓冲器测完即关，避免常驻电流 */
    SYSCFG->CFGR3 &= ~SYSCFG_CFGR3_ENBUF_VREFINT_ADC;

    if (raw == 0U) {
        return 0U;   /* 异常，避免除零 */
    }

    /* VDD = VREFINT_CAL × 3000 mV / VREFINT_DATA */
    uint32_t cal = (uint32_t)(*VREFINT_CAL_ADDR);
    return (cal * VREFINT_CAL_VREF_MV) / raw;
}

uint32_t battery_read_mv_filtered(uint8_t samples)
{
    if (samples == 0U) {
        return 0U;
    }

    uint32_t sum = 0U;
    uint8_t valid = 0U;
    for (uint8_t i = 0U; i < samples; i++) {
        uint32_t mv = battery_read_mv();
        if (mv > 1000U && mv < 4000U) {   /* 粗筛明显异常值 */
            sum += mv;
            valid++;
        }
    }

    return (valid == 0U) ? 0U : (sum / valid);
}

battery_level_t battery_classify(uint32_t vdd_mv)
{
    if (vdd_mv == 0U) {
        return BATTERY_LEVEL_UNKNOWN;
    }
    if (vdd_mv < BATTERY_THRESHOLD_CRITICAL_MV) {
        return BATTERY_LEVEL_CRITICAL;
    }
    if (vdd_mv < BATTERY_THRESHOLD_LOW_MV) {
        return BATTERY_LEVEL_LOW;
    }
    return BATTERY_LEVEL_NORMAL;
}
