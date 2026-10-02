/**
 * @file    clock_rtc.c
 * @brief   RTC 走时服务实现（寄存器直写，接口见 clock_rtc.h）。
 */
#include "clock_rtc.h"

#include <stddef.h>

#include "rtc_time.h"
#include "stm32l031xx.h"

/* RTC 备份寄存器约定：BKP0R 写 magic 表示"已对时"，BKP1R 存上次对时的
 * UTC 整秒。备份域由 VDD 供电（板上无 VBAT）：系统复位保留、换电池即失，
 * 与 RTC 走时本身的存续范围一致。 */
#define CLOCK_RTC_BKP_MAGIC  0x544F5450U /* 'TOTP' */

/* 首次初始化写入的默认日历：2001-01-01T00:00:00Z。
 * NOTE: 不用 2000-01-01 —— 年字段为 0 时 INITS 不置位（RM0367 §22.7.4），
 *       会让每次复位都重走初始化路径。 */
#define CLOCK_RTC_DEFAULT_UNIX_S 978307200ULL

/* LSE 起振等待上限：数据手册典型 <1 s、最大约 2 s，留 1.5 倍余量 */
#define CLOCK_RTC_LSE_TIMEOUT_MS     3000U
/* PWR_CR.DBP 写入后需要同步到备份域（HAL 同款等待，只防异常） */
#define CLOCK_RTC_DBP_TIMEOUT_MS     10U
/* 以下都是 RTC 时钟域内的几个周期，超时值只用于兜住硬件异常 */
#define CLOCK_RTC_INITF_TIMEOUT_MS   10U
#define CLOCK_RTC_RSF_TIMEOUT_MS     10U
#define CLOCK_RTC_SHPF_TIMEOUT_MS    20U
#define CLOCK_RTC_RECALPF_TIMEOUT_MS 100U

/* RTC_PRER：PREDIV_A = 127，PREDIV_S = 255 → 32768/(127+1)/256 = 1 Hz，
 * 亚秒分辨率 1/256 s（serial-protocol.md §5.4 的量化粒度） */
#define CLOCK_RTC_PREDIV_A 127U
#define CLOCK_RTC_PREDIV_S 255U

static bool s_rtc_ready;

/* ---- 有限等待：SysTick 轮询 ------------------------------------------- */

/* NOTE: 初始化与对时都是低频前台操作，用 SysTick 做毫秒级有限等待即可，
 *       不引入独立时基。轮询时 CPU 空转，最长一次是 LSE 冷启动（~1 s），
 *       折合电池消耗 <0.001 mAh，相对 CR2032 可忽略（architecture.md §4.1）。 */
static void systick_start_ms(void)
{
    uint32_t reload = SystemCoreClock / 1000U;
    if (reload == 0U) {
        reload = 1U;
    }
    SysTick->LOAD = reload - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    (void)SysTick->CTRL; /* 读一次清掉上一轮遗留的 COUNTFLAG */
}

static void systick_stop(void)
{
    SysTick->CTRL = 0U;
}

/** 在 timeout_ms 内等待 (*reg & mask) != 0；超时返回 false。 */
static bool wait_mask_set(volatile uint32_t *reg, uint32_t mask, uint32_t timeout_ms)
{
    uint32_t elapsed = 0U;
    systick_start_ms();
    while ((*reg & mask) == 0U) {
        if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U) {
            /* 读取 CTRL 时 COUNTFLAG 已清 */
            elapsed++;
            if (elapsed >= timeout_ms) {
                systick_stop();
                return false;
            }
        }
    }
    systick_stop();
    return true;
}

/** 在 timeout_ms 内等待 (*reg & mask) == 0；超时返回 false。 */
static bool wait_mask_clear(volatile uint32_t *reg, uint32_t mask, uint32_t timeout_ms)
{
    uint32_t elapsed = 0U;
    systick_start_ms();
    while ((*reg & mask) != 0U) {
        if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U) {
            elapsed++;
            if (elapsed >= timeout_ms) {
                systick_stop();
                return false;
            }
        }
    }
    systick_stop();
    return true;
}

/* ---- RCC_CSR 读改写与 RTC 访问 ---------------------------------------- */

/* NOTE: RCC_CSR 的 bit23（RMVF）与 bit25..31（复位原因标志）是"写 0 无副作用"
 *       的位。读-改-写必须把它们写成 0，否则会误清 POR/IWDG 等复位原因
 *       （DOT5 故障锁存依赖 IWDGRSTF，见 architecture.md §3.1）。 */
#define CLOCK_RTC_RCC_CSR_PRESERVE_MASK 0x007FFFFFU

static void rcc_csr_update(uint32_t set_bits, uint32_t clear_bits)
{
    uint32_t value = RCC->CSR & CLOCK_RTC_RCC_CSR_PRESERVE_MASK;
    value = (value & ~clear_bits) | set_bits;
    RCC->CSR = value;
}

/* RTC 寄存器写保护解锁序列（RM0367 §22.4.7；写错键即恢复保护） */
static void rtc_unlock(void)
{
    RTC->WPR = 0xCAU;
    RTC->WPR = 0x53U;
}

static void rtc_lock(void)
{
    RTC->WPR = 0xFFU;
}

static bool rtc_enter_init(void)
{
    rtc_unlock();
    RTC->ISR |= RTC_ISR_INIT;
    if (!wait_mask_set(&RTC->ISR, RTC_ISR_INITF, CLOCK_RTC_INITF_TIMEOUT_MS)) {
        rtc_lock();
        return false;
    }
    return true;
}

static void rtc_exit_init(void)
{
    RTC->ISR &= ~RTC_ISR_INIT;
}

/* ---- 日历寄存器编解码 ------------------------------------------------ */

static uint32_t encode_time_reg(const rtc_time_civil_t *civil)
{
    return ((uint32_t)rtc_time_bin_to_bcd(civil->hour) << RTC_TR_HU_Pos) |
           ((uint32_t)rtc_time_bin_to_bcd(civil->minute) << RTC_TR_MNU_Pos) |
           ((uint32_t)rtc_time_bin_to_bcd(civil->second) << RTC_TR_SU_Pos);
}

static uint32_t encode_date_reg(const rtc_time_civil_t *civil)
{
    return ((uint32_t)rtc_time_bin_to_bcd((uint8_t)(civil->year - RTC_TIME_YEAR_MIN))
            << RTC_DR_YT_Pos) |
           ((uint32_t)rtc_time_bin_to_bcd(civil->month) << RTC_DR_MU_Pos) |
           ((uint32_t)rtc_time_bin_to_bcd(civil->day) << RTC_DR_DU_Pos) |
           ((uint32_t)civil->weekday << RTC_DR_WDU_Pos);
}

static bool decode_civil(uint32_t tr, uint32_t dr, rtc_time_civil_t *civil)
{
    uint8_t hour = 0U;
    uint8_t minute = 0U;
    uint8_t second = 0U;
    uint8_t year = 0U;
    uint8_t month = 0U;
    uint8_t day = 0U;
    const uint8_t weekday = (uint8_t)((dr & RTC_DR_WDU_Msk) >> RTC_DR_WDU_Pos);

    if (!rtc_time_bcd_to_bin((uint8_t)((tr & RTC_TR_HU_Msk) >> RTC_TR_HU_Pos), &hour) ||
        !rtc_time_bcd_to_bin((uint8_t)((tr & RTC_TR_MNU_Msk) >> RTC_TR_MNU_Pos), &minute) ||
        !rtc_time_bcd_to_bin((uint8_t)(tr & RTC_TR_SU_Msk), &second) ||
        !rtc_time_bcd_to_bin((uint8_t)((dr & RTC_DR_YT_Msk) >> RTC_DR_YT_Pos), &year) ||
        !rtc_time_bcd_to_bin((uint8_t)((dr & RTC_DR_MU_Msk) >> RTC_DR_MU_Pos), &month) ||
        !rtc_time_bcd_to_bin((uint8_t)(dr & RTC_DR_DU_Msk), &day)) {
        return false;
    }
    if (hour > 23U || minute > 59U || second > 59U || weekday < 1U || weekday > 7U) {
        return false;
    }

    civil->year    = (uint16_t)(RTC_TIME_YEAR_MIN + year);
    civil->month   = month;
    civil->day     = day;
    civil->weekday = weekday;
    civil->hour    = hour;
    civil->minute  = minute;
    civil->second  = second;

    uint64_t unix_s = 0U;
    return rtc_time_to_unix(civil, &unix_s); /* 复用月份/闰年等范围校验 */
}

/* ---- 时间读取 -------------------------------------------------------- */

static bool rtc_read_ms(uint64_t *out_ms)
{
    /* 先清 RSF 再等：兼容从 Stop 唤醒后影子寄存器未更新的情况
     * （RM0367 §22.4.8）；RSF 是"写 0 清除"位，写值里同时保持 INIT 不变。 */
    RTC->ISR = ~(RTC_ISR_RSF | RTC_ISR_INIT) | (RTC->ISR & RTC_ISR_INIT);
    if (!wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS)) {
        return false;
    }

    /* 读 SSR/TR 会锁定日历影子寄存器，直到读 DR 解锁（RM0367 §22.4.8），
     * 因此三者是同一时刻的一致性快照 */
    const uint32_t ssr = RTC->SSR & RTC_SSR_SS;
    const uint32_t tr = RTC->TR;
    const uint32_t dr = RTC->DR;

    rtc_time_civil_t civil;
    uint64_t unix_s = 0U;
    if (!decode_civil(tr, dr, &civil) || !rtc_time_to_unix(&civil, &unix_s)) {
        return false;
    }

    /* SSR 是同步预分频计数器，向下计数：已过相位 = 255 - SSR。
     * 移位操作后 SS 可大于 255，此时相位为负，含义是"比日历秒字段早" */
    const int32_t ticks = (int32_t)RTC_TIME_TICKS_MAX - (int32_t)ssr;
    const int32_t ms = rtc_time_ms_from_ticks(ticks);
    *out_ms = unix_s * 1000U + (uint64_t)(int64_t)ms;
    return true;
}

/* ---- 对时状态 -------------------------------------------------------- */

static bool time_was_set(void)
{
    return s_rtc_ready && RTC->BKP0R == CLOCK_RTC_BKP_MAGIC;
}

/* ---- 对外接口 -------------------------------------------------------- */

bool clock_rtc_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP; /* 备份域写保护总开关，运行期保持打开 */
    /* DBP 在备份域侧生效前不能写 RCC_CSR 的 RTCSEL/RTCEN 与 RTC 寄存器 */
    if (!wait_mask_set(&PWR->CR, PWR_CR_DBP, CLOCK_RTC_DBP_TIMEOUT_MS)) {
        return false;
    }

    const bool lse_selected = (RCC->CSR & RCC_CSR_RTCSEL) == RCC_CSR_RTCSEL_LSE;
    const bool rtc_enabled = (RCC->CSR & RCC_CSR_RTCEN) != 0U;

    if (!lse_selected) {
        /* 首次上电/换电池：复位 RTC 域，使 RTCSEL 回到可编程状态，
         * 同时清掉可能残留的备份寄存器。走时本就不存在，无损失 */
        rcc_csr_update(RCC_CSR_RTCRST, 0U);
        rcc_csr_update(0U, RCC_CSR_RTCRST);
    }

    /* LSE 低驱动档（architecture.md §4.5；22 pF 负载的实机标定见 §11）；
     * 清 LSEBYP：本板是 32.768 kHz 晶体，不走外部时钟输入 */
    rcc_csr_update(RCC_CSR_LSEON, RCC_CSR_LSEDRV | RCC_CSR_LSEBYP);
    if (!wait_mask_set(&RCC->CSR, RCC_CSR_LSERDY, CLOCK_RTC_LSE_TIMEOUT_MS)) {
        return false;
    }

    if (!lse_selected) {
        rcc_csr_update(RCC_CSR_RTCSEL_LSE | RCC_CSR_RTCEN, 0U);
    } else if (!rtc_enabled) {
        rcc_csr_update(RCC_CSR_RTCEN, 0U);
    }

    if ((RTC->ISR & RTC_ISR_INITS) != 0U) {
        /* 系统复位（IWDG/软件）不打断 LSE 走时：沿用当前日历 */
        s_rtc_ready = true;
        return true;
    }

    /* 首次配置：分频 + 默认日历；NEVER_SET 由备份 magic 缺失隐含表达 */
    rtc_time_civil_t civil;
    if (!rtc_time_from_unix(CLOCK_RTC_DEFAULT_UNIX_S, &civil)) {
        return false;
    }
    if (!rtc_enter_init()) {
        return false;
    }
    RTC->PRER = ((uint32_t)CLOCK_RTC_PREDIV_A << RTC_PRER_PREDIV_A_Pos) |
                (uint32_t)CLOCK_RTC_PREDIV_S;
    RTC->TR = encode_time_reg(&civil);
    RTC->DR = encode_date_reg(&civil);
    rtc_exit_init();
    rtc_lock();

    if (!wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS)) {
        return false;
    }
    s_rtc_ready = true;
    return true;
}

bool clock_rtc_ready(void)
{
    return s_rtc_ready;
}

uint64_t clock_rtc_now_ms(void)
{
    uint64_t now = 0U;
    if (!s_rtc_ready || !rtc_read_ms(&now)) {
        return 0U;
    }
    return now;
}

uint8_t clock_rtc_time_flags(void)
{
    if (!s_rtc_ready) {
        return CLOCK_RTC_TIME_NEVER_SET;
    }
    if (!time_was_set()) {
        return CLOCK_RTC_TIME_NEVER_SET;
    }

    const uint64_t now = clock_rtc_now_ms();
    const uint32_t now_s = (uint32_t)(now / 1000U);
    const uint32_t last_s = RTC->BKP1R;
    if (now == 0U || now_s < last_s || (now_s - last_s) > CLOCK_RTC_STALE_AFTER_S) {
        return CLOCK_RTC_TIME_STALE;
    }
    return CLOCK_RTC_TIME_TRUSTED;
}

uint32_t clock_rtc_last_set_age_s(void)
{
    if (!time_was_set()) {
        return UINT32_MAX;
    }
    const uint64_t now = clock_rtc_now_ms();
    const uint32_t now_s = (uint32_t)(now / 1000U);
    const uint32_t last_s = RTC->BKP1R;
    if (now == 0U || now_s < last_s) {
        return 0U;
    }
    return now_s - last_s;
}

bool clock_rtc_set_unix_ms(uint64_t unix_ms, uint64_t *readback_ms)
{
    const uint64_t unix_s = unix_ms / 1000U;
    const uint16_t ms = (uint16_t)(unix_ms % 1000U);
    rtc_time_civil_t civil;
    uint8_t ticks = 0U;

    if (!s_rtc_ready) {
        return false;
    }
    if (!rtc_time_from_unix(unix_s, &civil)) {
        return false; /* 仅支持 2000..2099 */
    }
    if (!rtc_time_ticks_from_ms(ms, &ticks)) {
        return false;
    }

    /* 1. 写入整秒（init 模式下日历计数器停止，TR/DR 生效后重新起走） */
    if (!rtc_enter_init()) {
        return false;
    }
    RTC->TR = encode_time_reg(&civil);
    RTC->DR = encode_date_reg(&civil);
    rtc_exit_init();
    rtc_lock();
    if (!wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS)) {
        return false;
    }

    /* 2. 亚秒对齐：写 TR/DR 不改变同步预分频计数器的相位，用 SHIFTR 把
     *    当前相位修正到目标相位 ticks。RTC_SSR 只读，这是唯一定位亚秒的
     *    手段（RM0367 §22.4.10）。 */
    const uint32_t ssr = RTC->SSR & RTC_SSR_SS;
    /* NOTE: 读 SSR/TR 会锁定日历影子寄存器直到读 DR（RM0367 §22.4.8）；
     *       这里只取相位，读到 SSR 后立即读 DR 解锁，否则后续 RSF 永远
     *       不会重新置位（SHIFTR 会清 RSF）。 */
    (void)RTC->DR;
    if ((ssr & 0x8000U) != 0U) {
        return false; /* RM 要求 SS[15] 置位时禁止移位 */
    }
    int32_t delta = (int32_t)ticks - ((int32_t)RTC_TIME_TICKS_MAX - (int32_t)ssr);
    if (delta > 255 || delta < -255) {
        /* 上一次对时走了"提前"移位时，SS 会暂时大于 PREDIV_S（约 1 s 内
         * 自然消解），此时紧接的第二次对时可能超出单次移位范围：
         * 不写备份、报失败，由上位机的超时重传覆盖（serial-protocol.md §7），
         * 几百毫秒后的重试必然落在范围内。 */
        return false;
    }
    if (delta != 0) {
        rtc_unlock();
        if (delta > 0) {
            /* 提前 delta/256 s：ADD1S 加 1 s 再减去 (256-delta)/256 s */
            RTC->SHIFTR = RTC_SHIFTR_ADD1S | (uint32_t)(256 - delta);
        } else {
            /* 滞后 |delta|/256 s */
            RTC->SHIFTR = (uint32_t)(-delta);
        }
        rtc_lock();
        if (!wait_mask_clear(&RTC->ISR, RTC_ISR_SHPF, CLOCK_RTC_SHPF_TIMEOUT_MS)) {
            return false;
        }
        if (!wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS)) {
            return false;
        }
    }

    /* 3. 回读自检；达标才把"上次对时"写入备份寄存器 */
    uint64_t now = 0U;
    if (!rtc_read_ms(&now)) {
        return false;
    }
    if (readback_ms != NULL) {
        *readback_ms = now;
    }
    const uint64_t diff = (now >= unix_ms) ? (now - unix_ms) : (unix_ms - now);
    if (diff > CLOCK_RTC_SET_TOLERANCE_MS) {
        return false;
    }

    RTC->BKP0R = CLOCK_RTC_BKP_MAGIC;
    RTC->BKP1R = (uint32_t)unix_s; /* 2000..2099 年，远小于 2^32 */
    return true;
}

bool clock_rtc_set_calib(int16_t centi_ppm, int16_t *applied_centi_ppm)
{
    rtc_time_calib_t regs;
    if (!s_rtc_ready || !rtc_time_calib_from_centi_ppm(centi_ppm, &regs)) {
        return false;
    }
    if (!wait_mask_clear(&RTC->ISR, RTC_ISR_RECALPF, CLOCK_RTC_RECALPF_TIMEOUT_MS)) {
        return false;
    }

    rtc_unlock();
    RTC->CALR = (uint32_t)((regs.plus ? RTC_CALR_CALP : 0U) | (uint32_t)regs.minus);
    rtc_lock();
    /* 等待新校准被载入（RECALPF 置位后自动清除，约 2 个 RTCCLK） */
    if (!wait_mask_clear(&RTC->ISR, RTC_ISR_RECALPF, CLOCK_RTC_RECALPF_TIMEOUT_MS)) {
        return false;
    }

    rtc_time_calib_t readback;
    readback.plus = (RTC->CALR & RTC_CALR_CALP) != 0U;
    readback.minus = (uint16_t)(RTC->CALR & RTC_CALR_CALM);
    const int32_t applied = rtc_time_calib_to_centi_ppm(&readback);
    if (applied > INT16_MAX || applied < INT16_MIN) {
        return false;
    }
    if (applied_centi_ppm != NULL) {
        *applied_centi_ppm = (int16_t)applied;
    }
    return true;
}

bool clock_rtc_calib_active(void)
{
    return s_rtc_ready &&
           (RTC->CALR & (RTC_CALR_CALP | RTC_CALR_CALM)) != 0U;
}
