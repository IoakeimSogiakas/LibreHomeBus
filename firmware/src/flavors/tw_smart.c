/*
 * FID 4: tunable white on a TPS92520-Q1. Same registers as FID 5.
 *   reg 1: on/off
 *   reg 2: cold 0..1000  (TPS channel 1)
 *   reg 3: warm 0..1000  (TPS channel 2)
 *
 * IADJ sets the LED current once at init, brightness is the TPS internal
 * PWM. CHx_EN drive UDIM and hard-gate each channel.
 *
 * Every pass sends at most one SPI frame (a read is two, ~50 us) so the
 * UART never misses a byte. Only tps_init() blocks longer (~1 ms).
 */

#include "node.h"
#include "tps92520.h"

#if FLAVOR == FID_TW_SMART

#define CH1_EN_PIN    PD2      /* UDIM1, cold */
#define CH2_EN_PIN    PC3      /* UDIM2, warm */
#define FLT_PIN       PD3      /* !FLT, open drain, active low */

#define RCS_MOHM      330      /* R2/R3, milliohm. Must match the board */
#define LED_MA        340      /* LED current per channel */
#define TON           7        /* ~437 kHz switching */
#define PWMDIV        0        /* 1507 Hz PWM */

#define REG_ONOFF     1
#define REG_COLD      2
#define REG_WARM      3
#define LEVEL_MAX     1000

#define STATUS_MS     200
#define RETRY_MS      500

/* I_LED = 2.45 V * IADJ / 1024 / (14 * Rcs) */
#define IADJ  ((LED_MA * 14ULL * RCS_MOHM * 1024 + 1225000) / 2450000)

#if IADJ > 1023
#error "LED_MA is above what RCS_MOHM allows"
#endif
#if LED_MA > 1600
#error "TPS92520 is rated 1.6 A per channel"
#endif

static const uint8_t en_pin[2]  = { CH1_EN_PIN, CH2_EN_PIN };
static const uint8_t pwm_l[2]   = { TPS_CH1PWML, TPS_CH2PWML };
static const uint8_t pwm_h[2]   = { TPS_CH1PWMH, TPS_CH2PWMH };

static uint8_t  ready;
static uint16_t cur[2];        /* duty the TPS has */
static uint8_t  step[2];       /* 0 idle, 1 high byte sent */
static uint16_t next[2];
static uint8_t  st_step;
static uint8_t  last_s1, last_s2, last_tw;
static uint32_t t_status, t_retry;

static uint16_t duty(uint16_t level)
{
    return (uint16_t)(((uint32_t)level * TPS_PWM_MAX + LEVEL_MAX / 2) / LEVEL_MAX);
}

static void stop(void)
{
    funDigitalWrite(CH1_EN_PIN, FUN_LOW);
    funDigitalWrite(CH2_EN_PIN, FUN_LOW);
    ready = 0;
    t_retry = SysTick->CNT;
}

static void start(void)
{
    if (!tps_init(IADJ, IADJ, TON, PWMDIV)) {
        t_retry = SysTick->CNT;
        return;
    }
    ready = 1;
    cur[0] = cur[1] = 0;
    step[0] = step[1] = 0;
    st_step = 0;
    t_status = SysTick->CNT;
    debugln("tps ready, iadj %u", (unsigned)IADJ);
}

/* One SPI frame per call. Returns 1 if it used the bus. */
static int channel_poll(int ch, uint16_t want)
{
    if (step[ch] == 0) {
        if (want == cur[ch]) return 0;
        if (want == 0) funDigitalWrite(en_pin[ch], FUN_LOW);
        next[ch] = want;
        tps_write(pwm_h[ch], next[ch] >> 8);
        step[ch] = 1;
        return 1;
    }
    tps_write(pwm_l[ch], next[ch] & 0xFF);     /* low byte latches the value */
    cur[ch] = next[ch];
    step[ch] = 0;
    if (cur[ch]) funDigitalWrite(en_pin[ch], FUN_HIGH);
    return 1;
}

static void status_poll(void)
{
    int v;

    switch (st_step) {
    case 0:
        if ((uint32_t)(SysTick->CNT - t_status) < STATUS_MS * DELAY_MS_TIME) return;
        t_status = SysTick->CNT;
        v = tps_read(TPS_STATUS3);
        if (v < 0 || (v & (TPS_STANDALONE | TPS_PC | TPS_CMWTO))) {
            debugln("tps lost (status3 %d), restarting", v);
            stop();
            return;
        }
        if (!!(v & TPS_TW) != last_tw) {
            last_tw = !!(v & TPS_TW);
            debugln("tps thermal warning %s", last_tw ? "on" : "off");
        }
        if ((v & (TPS_CH1STATUS | TPS_CH2STATUS)) || !funDigitalRead(FLT_PIN)) {
            st_step = 1;
        }
        return;
    case 1:
        v = tps_read(TPS_STATUS1);
        if (v >= 0 && v != last_s1) {
            last_s1 = v;
            debugln("tps status1 %02X", v);
        }
        st_step = 2;
        return;
    case 2:
        v = tps_read(TPS_STATUS2);
        if (v >= 0 && v != last_s2) {
            last_s2 = v;
            debugln("tps status2 %02X", v);
        }
        st_step = 3;
        return;
    default:
        tps_write(TPS_SYSCFG1, TPS_CMWEN | TPS_CH1INTPWM | TPS_CH1EN |
                               TPS_CH2INTPWM | TPS_CH2EN | TPS_FPINRST);
        st_step = 0;
        return;
    }
}

void flavor_init(void)
{
    funPinMode(CH1_EN_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funPinMode(CH2_EN_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(CH1_EN_PIN, FUN_LOW);
    funDigitalWrite(CH2_EN_PIN, FUN_LOW);
    funPinMode(FLT_PIN, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(FLT_PIN, FUN_HIGH);

    regs[REG_ONOFF] = 0;
    regs[REG_COLD]  = LEVEL_MAX;
    regs[REG_WARM]  = LEVEL_MAX;
    reg_max[REG_ONOFF] = 1;
    reg_max[REG_COLD]  = LEVEL_MAX;
    reg_max[REG_WARM]  = LEVEL_MAX;

    tps_spi_init();
    start();            /* must run within ~1.5 s of TPS power-up */
}

void flavor_poll(void)
{
    if (!ready) {
        /* tps_init blocks ~1 ms, don't start it while a frame is coming in */
        if (rx_len == 0 &&
            (uint32_t)(SysTick->CNT - t_retry) > RETRY_MS * DELAY_MS_TIME) {
            start();
        }
        return;
    }

    uint16_t on = regs[REG_ONOFF];
    uint16_t want0 = on ? duty(regs[REG_COLD]) : 0;
    uint16_t want1 = on ? duty(regs[REG_WARM]) : 0;

    if (step[1] == 0 && channel_poll(0, want0)) return;
    if (channel_poll(1, want1)) return;
    status_poll();
}

#endif /* FLAVOR == FID_TW_SMART */
