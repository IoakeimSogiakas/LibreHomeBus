/*
 * Device flavors, picked at build time with -DFLAVOR=<n>.
 * Each flavor provides flavor_init() (pins, peripherals, register setup)
 * and flavor_poll() (called every main loop pass, syncs regs and hardware).
 */

#include "node.h"


/* ---- shared by FID_SWITCH and FID_PLUG --------------------------------- */

#if FLAVOR == FID_SWITCH || FLAVOR == FID_PLUG

#define SW_PIN          PC1
#define DEBOUNCE_MS     20

#define LED_PIN         PC2
#define LED_ACTIVE      1
#define LED_TIMEOUT_MS  30000UL
#define REG_LED         2

static uint8_t  sw_raw_last;
static uint8_t  sw_stable;
static uint32_t sw_changed;

static void sw_init(void)
{
    funPinMode(SW_PIN, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(SW_PIN, FUN_HIGH);

    /* active-low input, so invert the reading */
    sw_raw_last = sw_stable = !funDigitalRead(SW_PIN);
}

/* Returns 1 when the debounced state just changed. */
static int sw_poll(void)
{
    uint8_t raw = !funDigitalRead(SW_PIN);

    if (raw != sw_raw_last) {
        sw_raw_last = raw;
        sw_changed  = SysTick->CNT;
    }
    else if (raw != sw_stable &&
             (uint32_t)(SysTick->CNT - sw_changed) > (DEBOUNCE_MS * DELAY_MS_TIME)) {
        sw_stable = raw;
        return 1;
    }

    return 0;
}

static void led_init(void)
{
    funPinMode(LED_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(LED_PIN, !LED_ACTIVE);

    regs[REG_LED] = 0;
    reg_max[REG_LED] = 1;
}

static void led_poll(void)
{
    static uint16_t last;
    static uint32_t on_since;

    if (regs[REG_LED] != last) {
        last = regs[REG_LED];
        funDigitalWrite(LED_PIN, last ? LED_ACTIVE : !LED_ACTIVE);
        on_since = SysTick->CNT;
    }
    /* switch the LED off if the master leaves it on */
    else if (last && (uint32_t)(SysTick->CNT - on_since) > (LED_TIMEOUT_MS * DELAY_MS_TIME)) {
        regs[REG_LED] = 0;
    }
}

#endif /* FLAVOR == FID_SWITCH || FLAVOR == FID_PLUG */


/* ---- flavors ----------------------------------------------------------- */

#if FLAVOR == FID_SWITCH
/* reg 1: switch state, 1 = on (read-only)
 * reg 2: identification LED (read/write), only the commissioning script uses it */
#define REG_SW_STATE  1

void flavor_init(void)
{
    sw_init();
    led_init();
    regs[REG_SW_STATE] = sw_stable;
}

void flavor_poll(void)
{
    if (sw_poll()) {
        regs[REG_SW_STATE] = sw_stable;
        debugln("switch %s", sw_stable ? "ON" : "OFF");
    }
    led_poll();
}

#elif FLAVOR == FID_LIGHT_ONOFF
/* reg 1: on/off (read/write) */
#define LIGHT_PIN     PD2
#define LIGHT_ACTIVE  1        /* pin level that turns the light on */
#define REG_ONOFF     1

static uint16_t last_state = 0xFFFF;   /* impossible value, forces the first update */

void flavor_init(void)
{
    funPinMode(LIGHT_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(LIGHT_PIN, !LIGHT_ACTIVE);
    regs[REG_ONOFF] = 0;
    reg_max[REG_ONOFF] = 1;
}

void flavor_poll(void)
{
    if (regs[REG_ONOFF] != last_state) {
        last_state = regs[REG_ONOFF];
        funDigitalWrite(LIGHT_PIN, regs[REG_ONOFF] ? LIGHT_ACTIVE : !LIGHT_ACTIVE);
        debugln("light %s", last_state ? "ON" : "OFF");
    }
}

#elif FLAVOR == FID_LIGHT_DIM
/* reg 1: on/off (read/write)
 * reg 2: brightness 0..1000 (read/write) */
#define PWM_PIN       PD2      /* TIM1 CH1 */
#define REG_ONOFF     1
#define REG_BRT       2
#define PWM_STEPS     1000

static uint16_t last_duty = 0xFFFF;

void flavor_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
    funPinMode(PWM_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);

    TIM1->PSC   = 0;
    TIM1->ATRLR = PWM_STEPS - 1;        /* 48 MHz / 1000 = 48 kHz */
    TIM1->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;   /* PWM mode 1, buffered */
    TIM1->CCER  |= TIM_CC1E;
    TIM1->BDTR  |= TIM_MOE;             /* TIM1 outputs stay off without this */
    TIM1->CH1CVR = 0;
    TIM1->SWEVGR |= TIM_UG;
    TIM1->CTLR1 |= TIM_CEN;

    regs[REG_ONOFF] = 0;
    regs[REG_BRT]   = PWM_STEPS;
    reg_max[REG_ONOFF] = 1;
    reg_max[REG_BRT]   = PWM_STEPS;
}

void flavor_poll(void)
{
    uint16_t duty = regs[REG_ONOFF] ? regs[REG_BRT] : 0;
    if (duty != last_duty) {
        last_duty = duty;
        TIM1->CH1CVR = duty;
        debugln("duty %u", duty);
    }
}

#elif FLAVOR == FID_TW_PWM
/* Tunable white, one PWM per channel into the LED driver DIM pins.
 * The master works out the cold/warm mix, this chip just sets the two levels.
 * reg 1: on/off (read/write)
 * reg 2: cold 0..1000 (read/write)
 * reg 3: warm 0..1000 (read/write) */
#define PWM_COLD_PIN  PD2      /* TIM1 CH1 */
#define PWM_WARM_PIN  PC3      /* TIM1 CH3 */
#define REG_ONOFF     1
#define REG_COLD      2
#define REG_WARM      3
#define PWM_STEPS     1000
#define PWM_PSC       47       /* 48 MHz / 48 / 1000 = 1 kHz */

static uint16_t last_cold = 0xFFFF;
static uint16_t last_warm = 0xFFFF;

void flavor_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
    funPinMode(PWM_COLD_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);
    funPinMode(PWM_WARM_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);

    TIM1->PSC   = PWM_PSC;
    TIM1->ATRLR = PWM_STEPS - 1;
    TIM1->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;
    TIM1->CHCTLR2 |= TIM_OC3M_2 | TIM_OC3M_1 | TIM_OC3PE;
    TIM1->CCER  |= TIM_CC1E | TIM_CC3E;
    TIM1->BDTR  |= TIM_MOE;
    TIM1->CH1CVR = 0;
    TIM1->CH3CVR = 0;
    TIM1->SWEVGR |= TIM_UG;
    TIM1->CTLR1 |= TIM_CEN;

    regs[REG_ONOFF] = 0;
    regs[REG_COLD]  = PWM_STEPS;
    regs[REG_WARM]  = PWM_STEPS;
    reg_max[REG_ONOFF] = 1;
    reg_max[REG_COLD]  = PWM_STEPS;
    reg_max[REG_WARM]  = PWM_STEPS;
}

void flavor_poll(void)
{
    uint16_t cold = regs[REG_ONOFF] ? regs[REG_COLD] : 0;
    uint16_t warm = regs[REG_ONOFF] ? regs[REG_WARM] : 0;
    if (cold != last_cold || warm != last_warm) {
        last_cold = cold;
        last_warm = warm;
        TIM1->CH1CVR = cold;
        TIM1->CH3CVR = warm;
        debugln("cold %u warm %u", cold, warm);
    }
}

#elif FLAVOR == FID_PLUG
/* reg 1: relay state, 1 = closed/on (read/write, the switch input also sets it)
 * reg 2: identification LED, see the shared code above */
#define RELAY_PIN     PD2
#define RELAY_ACTIVE  1
#define REG_ONOFF     1

static uint16_t last_state = 0xFFFF;

void flavor_init(void)
{
    funPinMode(RELAY_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(RELAY_PIN, !RELAY_ACTIVE);
    reg_max[REG_ONOFF] = 1;
    sw_init();
    regs[REG_ONOFF] = sw_stable;
    led_init();
}

void flavor_poll(void)
{
    if (sw_poll()) {
        regs[REG_ONOFF] = sw_stable;
    }
    if (regs[REG_ONOFF] != last_state) {
        last_state = regs[REG_ONOFF];
        funDigitalWrite(RELAY_PIN, last_state ? RELAY_ACTIVE : !RELAY_ACTIVE);
        debugln("relay %s", last_state ? "ON" : "OFF");
    }
    led_poll();
}

#else
/* FID_NONE or not implemented yet */
void flavor_init(void) {}
void flavor_poll(void) {}
#endif
