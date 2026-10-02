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
/* NOTE: 纯 SUBFS 回退下 SHPF（移位挂起）约 2 个亚秒 tick（~7.8 ms）后随
 *       移位生效一起清除；超时按最长一个亚秒周期取 1100 ms，仅用于兜住
 *       硬件异常 —— 若 SHPF 真停留到秒边界，SET_TIME 端到端会接近 1 s，
 *       而实机 6 轮实测响应均为 40 ms，正常路径远快于此。 */
#define CLOCK_RTC_SHPF_TIMEOUT_MS    1100U
/* SHIFTR 写入到移位生效的挂起时长，单位 1/256 s（实机实测量：恒为 2） */
#define CLOCK_RTC_SHIFT_PENDING_TICKS 2U
#define CLOCK_RTC_RECALPF_TIMEOUT_MS 100U
#define CLOCK_RTC_ALRAWF_TIMEOUT_MS  20U

/* RTC 闹钟内部唤醒线：EXTI 线 17（RM0367 §13.3.2 唤醒事件连接表） */
#define CLOCK_RTC_ALARM_EXTI_LINE    (1UL << 17)

/* RTC_PRER：PREDIV_A = 127，PREDIV_S = 255 → 32768/(127+1)/256 = 1 Hz，
 * 亚秒分辨率 1/256 s（量化粒度） */
#define CLOCK_RTC_PREDIV_A 127U
#define CLOCK_RTC_PREDIV_S 255U

static bool s_rtc_ready;

/* ---- 有限等待：SysTick 轮询 ------------------------------------------- */

/* NOTE: 初始化与对时都是低频前台操作，用 SysTick 做毫秒级有限等待即可，
 *       不引入独立时基。轮询时 CPU 空转，最长一次是 LSE 冷启动（~1 s），
 *       折合电池消耗 <0.001 mAh，相对 CR2032 可忽略。 */
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
 *       （DOT5 故障锁存依赖 IWDGRSTF）。 */
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
    /* NOTE: 十位字段（YT/MT/DT）在寄存器里正好位于个位字段上方 4 bit，
     *       所以整个两位 BCD 应放到个位字段的位置：年份放 YU_Pos，
     *       月放 MU_Pos，日放 DU_Pos。放到十位字段会整体左移 4 bit
     *       （2026 写成 "60"，实机 2026-10-02 复现）。 */
    return ((uint32_t)rtc_time_bin_to_bcd((uint8_t)(civil->year - RTC_TIME_YEAR_MIN))
            << RTC_DR_YU_Pos) |
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

    /* NOTE: 每个字段取"十位+个位"合成的整字节 BCD（十位就在个位上方 4 bit），
     *       只取个位半字节会把 10 月/40 分这类两位值解成个位数字
     *       （实机 2026-10-02 因此 set-time 回读恒失败）。掩码只覆盖
     *       十位+个位，天然挡掉保留位。 */
    if (!rtc_time_bcd_to_bin((uint8_t)((tr >> RTC_TR_HU_Pos) & 0x3FU), &hour) ||
        !rtc_time_bcd_to_bin((uint8_t)((tr >> RTC_TR_MNU_Pos) & 0x7FU), &minute) ||
        !rtc_time_bcd_to_bin((uint8_t)(tr & 0x7FU), &second) ||
        !rtc_time_bcd_to_bin((uint8_t)((dr >> RTC_DR_YU_Pos) & 0xFFU), &year) ||
        !rtc_time_bcd_to_bin((uint8_t)((dr >> RTC_DR_MU_Pos) & 0x1FU), &month) ||
        !rtc_time_bcd_to_bin((uint8_t)(dr & 0x3FU), &day)) {
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

/* ---- 日历写入与亚秒移位 ---------------------------------------------- */

/** init 模式下写入整份日历并等待影子寄存器同步（RSF）；RTC 异常返回 false。 */
static bool rtc_write_civil(const rtc_time_civil_t *civil)
{
    if (!rtc_enter_init()) {
        return false;
    }
    RTC->TR = encode_time_reg(civil);
    RTC->DR = encode_date_reg(civil);
    rtc_exit_init();
    rtc_lock();
    return wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS);
}

/** 读当前亚秒相位（已过 1/256 s 数，0..255）。SS 超过 PREDIV_S
 *  （上次移位的余波，相位为负）返回 false，由上位机重试覆盖。 */
static bool rtc_read_phase_ticks(uint32_t *ticks_out)
{
    const uint32_t ssr = RTC->SSR & RTC_SSR_SS;
    /* NOTE: 读 SSR 会锁定日历影子寄存器直到读 DR（RM0367 §22.4.8），
     *       这里只取相位，立即读 DR 解锁。 */
    (void)RTC->DR;
    if (ssr > RTC_TIME_TICKS_MAX) {
        return false;
    }
    *ticks_out = RTC_TIME_TICKS_MAX - ssr;
    return true;
}

/** 纯回退移位：亚秒相位回退 subfs/256 s（subfs 取 1..255）。 */
static bool rtc_subfs_shift(uint32_t subfs)
{
    rtc_unlock();
    RTC->SHIFTR = subfs & RTC_SHIFTR_SUBFS_Msk;
    rtc_lock();
    if (!wait_mask_clear(&RTC->ISR, RTC_ISR_SHPF, CLOCK_RTC_SHPF_TIMEOUT_MS)) {
        return false;
    }
    return wait_mask_set(&RTC->ISR, RTC_ISR_RSF, CLOCK_RTC_RSF_TIMEOUT_MS);
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

    /* LSE 驱动档取中高（LSEDRV=10）。实机标定（2026-10-02，常温冷启动）：
     * 低档 LSEDRV=00 启振 6.0–6.3 s，超过 3 s 等待上限导致降级；中高档
     * 3 次实测均 ~300 ms。清 LSEBYP：本板是 32.768 kHz 晶体，非外部时钟。 */
    rcc_csr_update(RCC_CSR_LSEON | RCC_CSR_LSEDRV_1, RCC_CSR_LSEDRV | RCC_CSR_LSEBYP);
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

    /* 1. 写入目标整秒（init 模式下日历计数器停止，TR/DR 生效后重新起走） */
    if (!rtc_write_civil(&civil)) {
        return false;
    }

    /* 2. 亚秒对齐：init 写入会把亚秒计数器一并复位（实机验证：写入后
     *    SSR 恒为 PREDIV_S，即相位 0），因此只需把相位挪到目标值 X/256 s。
     *    NOTE: 实机实测（2026-10-02，L031 实物）：
     *     - SHIFTR 写入后要 2 个 1/256 s 周期（~7.8 ms）才生效，且生效时
     *       以当时的相位为基准（8 次探针一致）—— 所以计算回退量时必须把
     *       这段挂起时间补进去，否则回读恒超差；
     *     - ADD1S 位不产生 +1 s 效果，只做回退移位（SUBFS），需要"提前"
     *       时先把日历写为目标秒 + 1，再用 (1 s − 提前量) 抵消。 */
    uint32_t phase = 0U;
    if (!rtc_read_phase_ticks(&phase)) {
        phase = 0U;
    }
    uint32_t at_shift = phase + CLOCK_RTC_SHIFT_PENDING_TICKS;
    if (at_shift > (uint32_t)ticks) {
        if (!rtc_subfs_shift(at_shift - (uint32_t)ticks)) {
            return false;
        }
    } else if (at_shift < (uint32_t)ticks) {
        rtc_time_civil_t next;
        /* unix_s + 1 越界（2099-12-31T23:59:59Z 对时）时无法补偿：报失败 */
        if (!rtc_time_from_unix(unix_s + 1U, &next) || !rtc_write_civil(&next)) {
            return false;
        }
        phase = 0U;
        if (!rtc_read_phase_ticks(&phase)) {
            phase = 0U;
        }
        at_shift = phase + CLOCK_RTC_SHIFT_PENDING_TICKS;
        if (!rtc_subfs_shift(256U + at_shift - (uint32_t)ticks)) {
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

bool clock_rtc_alarm_set(uint64_t unix_ms)
{
    if (!s_rtc_ready) {
        return false;
    }

    uint64_t fire_s = unix_ms / 1000U;
    const uint64_t now_s = clock_rtc_now_ms() / 1000U;
    if (fire_s <= now_s) {
        fire_s = now_s + 1U;   /* 目标整秒已到：顺延，避免立即触发 */
    }

    rtc_time_civil_t civil;
    if (!rtc_time_from_unix(fire_s, &civil)) {
        return false;
    }

    rtc_unlock();
    RTC->CR &= ~RTC_CR_ALRAE;
    if (!wait_mask_set(&RTC->ISR, RTC_ISR_ALRAWF, CLOCK_RTC_ALRAWF_TIMEOUT_MS)) {
        rtc_lock();
        return false;
    }
    /* NOTE: 只清 ALRAF；ISR 的其它位是只读/写 1 触发位（如 INIT），
     *       与 clock_rtc_set_unix_ms 的写法保持同一模式。 */
    RTC->ISR = ~(RTC_ISR_ALRAF | RTC_ISR_INIT) | (RTC->ISR & RTC_ISR_INIT);
    RTC->ALRMAR = RTC_ALRMAR_MSK4 |               /* 日期不参与匹配 */
                  (uint32_t)rtc_time_bin_to_bcd(civil.second) |
                  ((uint32_t)rtc_time_bin_to_bcd(civil.minute) << RTC_ALRMAR_MNU_Pos) |
                  ((uint32_t)rtc_time_bin_to_bcd(civil.hour) << RTC_ALRMAR_HU_Pos);
    RTC->CR |= RTC_CR_ALRAE | RTC_CR_ALRAIE;
    rtc_lock();

    EXTI->PR = CLOCK_RTC_ALARM_EXTI_LINE;   /* 清残留挂起，避免假唤醒 */
    /* NOTE: RTC 闹钟在 EXTI 线 17 上按上升沿触发（RM0367 §13.3.2/§13.4.2），
     *       RTSR 与 IMR 必须同时配置：只开 IMR 时 Stop 下闹钟不会唤醒 MCU，
     *       设备空闲约 4 s 后被 IWDG 反复复位（FAULT_LATCHED + 复位原因 IWDG）。 */
    EXTI->RTSR |= CLOCK_RTC_ALARM_EXTI_LINE;
    EXTI->IMR |= CLOCK_RTC_ALARM_EXTI_LINE;
    NVIC_EnableIRQ(RTC_IRQn);
    return true;
}

/**
 * @brief  RTC 中断服务：只处理闹钟 A（本工程唯一的 RTC 中断源）。
 * @note   匹配字段在整秒内保持相等，若只清标志会重复触发；这里同时停用
 *         ALRAE，等待主循环下次入睡前重新武装。
 */
void RTC_IRQHandler(void)
{
    if ((RTC->ISR & RTC_ISR_ALRAF) != 0U) {
        rtc_unlock();
        RTC->CR &= ~RTC_CR_ALRAE;
        RTC->ISR = ~(RTC_ISR_ALRAF | RTC_ISR_INIT) | (RTC->ISR & RTC_ISR_INIT);
        rtc_lock();
        EXTI->PR = CLOCK_RTC_ALARM_EXTI_LINE;
    }
}
