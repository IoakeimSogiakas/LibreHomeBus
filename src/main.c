/*
 * Modbus RTU slave over RS-485 for the CH32V003, with automatic address
 * discovery. Needs the ch32fun framework (config.h pulls it in).
 *
 * One firmware, several flavors picked at build time. Register 0 always
 * holds the flavor ID (FID) so the master knows what it is talking to.
 *
 * Build flags:
 *   -DFLAVOR=<n>      device type, see FID_* below
 *   -DDEFAULT_ID=<n>  fixed address 1..247, or 0 for unassigned (default).
 *                     Unassigned chips ignore normal traffic, join discovery,
 *                     and keep the address they are given in flash.
 *
 * Discovery (broadcast address 0, custom function codes, no Modbus replies):
 *   00 41 <uid:8> crc          unassigned chips with UID <= this answer 0x55
 *   00 42 <uid:8> <addr> crc   chip with this UID takes <addr> and saves it
 *   00 43 <uid:8> crc          chip with this UID forgets its address
 */

#include "config.h"


/* ---- protocol ---------------------------------------------------------- */

#define FN_READ_HOLDING   0x03
#define FN_WRITE_SINGLE   0x06
/* 0x41-0x48 is reserved for user-defined functions in the Modbus spec,
 * so these can't collide with anything standard. */
#define FN_COMPARE        0x41
#define FN_ASSIGN         0x42
#define FN_FORGET         0x43

#define EX_ILLEGAL_FUNCTION  0x01
#define EX_ILLEGAL_ADDRESS   0x02
#define EX_ILLEGAL_VALUE     0x03

/* Master only checks that something arrived, the value doesn't matter. */
#define REPLY_BYTE        0x55

/* Exact frame lengths: address + function + payload + 2 CRC bytes. */
#define LEN_REQUEST       8
#define LEN_COMPARE       12
#define LEN_ASSIGN        13
#define LEN_FORGET        12

#define ID_BROADCAST      0      /* also means "unassigned" */
#define ID_MAX            247

#ifndef DEFAULT_ID
#define DEFAULT_ID        0
#endif


/* ---- hardware ---------------------------------------------------------- */

#define BAUD         115200
#define RX_BUF_SIZE  64
#define DE_PIN       PD4         /* DE and /RE tied together: high = send */

/* A frame ends after 3.5 character times of silence. The spec fixes it at
 * 1750 us for anything above 19200 baud. */
#define T35_US       ((BAUD > 19200) ? 1750 : (38500000UL / BAUD))
#define T35_TICKS    (T35_US * DELAY_US_TIME)


/* ---- flavor IDs -------------------------------------------------------- */

#define FID_NONE         0
#define FID_SWITCH       1
#define FID_LIGHT_ONOFF  2
#define FID_LIGHT_DIM    3
#define FID_TW_SMART     4   // tunable white, SPI driver (not done yet) */
#define FID_TW_PWM       5
#define FID_PLUG         6   //Not tested yet

#ifndef FLAVOR
#define FLAVOR FID_NONE
#endif


/* ---- registers --------------------------------------------------------- */

#define NUM_REGS     4

/* What the master sees. Flavor code keeps it in sync with the hardware. */
static uint16_t regs[NUM_REGS] = { FLAVOR };

/* Highest value a write may set. 0 = read-only. */
static uint16_t reg_max[NUM_REGS];


/* ---- state ------------------------------------------------------------- */

static uint8_t  rx_buf[RX_BUF_SIZE];
static uint8_t  rx_len;
static uint8_t  rx_overflow;
static uint32_t rx_last;               /* SysTick value at the last byte */

static uint8_t  slave_id = DEFAULT_ID;
static uint64_t my_uid;


/* ---- UART and framing -------------------------------------------------- */

static void uart_init(void)
{
    funGpioInitAll();
    RCC->APB2PCENR |= RCC_APB2Periph_USART1;

    funPinMode(PD5, GPIO_CFGLR_OUT_10Mhz_AF_PP);   /* TX */
    funPinMode(PD6, GPIO_CFGLR_IN_FLOAT);          /* RX */
    funPinMode(DE_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(DE_PIN, FUN_LOW);

    USART1->BRR   = (FUNCONF_SYSTEM_CORE_CLOCK + BAUD / 2) / BAUD;
    USART1->CTLR1 = USART_WordLength_8b | USART_Parity_No | USART_Mode_Tx | USART_Mode_Rx;
    USART1->CTLR2 = USART_StopBits_1;
    USART1->CTLR3 = USART_HardwareFlowControl_None;
    USART1->CTLR1 |= CTLR1_UE_Set;
}

/* Returns 1 when a complete frame is sitting in rx_buf. */
static int frame_poll(void)
{
    if (USART1->STATR & USART_FLAG_RXNE) {
        uint8_t b = USART1->DATAR;
        rx_last = SysTick->CNT;
        if (rx_len < RX_BUF_SIZE) {
            rx_buf[rx_len++] = b;
        } else {
            rx_overflow = 1;
        }
        /* The compare frame has a known length, so answer right away instead
         * of waiting out the silence. A search is ~64 of these in a row. */
        if (rx_len == LEN_COMPARE && rx_buf[0] == ID_BROADCAST && rx_buf[1] == FN_COMPARE) {
            return 1;
        }
    } else if (rx_len && (uint32_t)(SysTick->CNT - rx_last) > T35_TICKS) {
        return 1;
    }
    return 0;
}

static void tx_frame(const uint8_t *p, uint8_t n)
{
    funDigitalWrite(DE_PIN, FUN_HIGH);

    for (uint8_t i = 0; i < n; i++) {
        while (!(USART1->STATR & USART_FLAG_TXE));
        USART1->DATAR = p[i];
    }

    /* Wait for TC, not TXE: TXE goes high while the last byte is still
     * shifting out, and dropping DE then would cut it off. */
    while (!(USART1->STATR & USART_FLAG_TC));
    funDigitalWrite(DE_PIN, FUN_LOW);
}


/* ---- CRC-16 ------------------------------------------------------------ */

static uint16_t crc16(const uint8_t *p, uint8_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}

/* CRC goes on the wire low byte first. */
static int crc_good(void)
{
    uint16_t got = rx_buf[rx_len - 2] | (rx_buf[rx_len - 1] << 8);
    return got == crc16(rx_buf, rx_len - 2);
}


/* ---- normal Modbus requests -------------------------------------------- */

static void send_exception(uint8_t func, uint8_t code)
{
    uint8_t r[5] = { slave_id, (uint8_t)(func | 0x80), code, 0, 0 };   /* 0x80 = error reply */
    uint16_t c = crc16(r, 3);
    r[3] = c & 0xFF;
    r[4] = c >> 8;
    tx_frame(r, 5);
}

static void handle_read_holding(void)
{
    uint16_t addr  = (rx_buf[2] << 8) | rx_buf[3];
    uint16_t count = (rx_buf[4] << 8) | rx_buf[5];

    if (count < 1 || count > 125) {
        send_exception(FN_READ_HOLDING, EX_ILLEGAL_VALUE);
        return;
    }
    if (addr + count > NUM_REGS) {
        send_exception(FN_READ_HOLDING, EX_ILLEGAL_ADDRESS);
        return;
    }

    uint8_t r[3 + 2 * NUM_REGS + 2];
    uint8_t n = 0;

    r[n++] = slave_id;
    r[n++] = FN_READ_HOLDING;
    r[n++] = count * 2;

    for (uint16_t i = 0; i < count; i++) {
        r[n++] = regs[addr + i] >> 8;
        r[n++] = regs[addr + i] & 0xFF;
    }

    uint16_t c = crc16(r, n);
    r[n++] = c & 0xFF;
    r[n++] = c >> 8;
    tx_frame(r, n);
}

static void handle_write_single(void)
{
    uint16_t addr  = (rx_buf[2] << 8) | rx_buf[3];
    uint16_t value = (rx_buf[4] << 8) | rx_buf[5];

    if (addr >= NUM_REGS || reg_max[addr] == 0) {
        send_exception(FN_WRITE_SINGLE, EX_ILLEGAL_ADDRESS);
        return;
    }
    if (value > reg_max[addr]) {
        send_exception(FN_WRITE_SINGLE, EX_ILLEGAL_VALUE);
        return;
    }
    regs[addr] = value;
    tx_frame(rx_buf, LEN_REQUEST);     /* echoing the request is the ack */
}

static void handle_frame(void)
{
    switch (rx_buf[1]) {
    case FN_READ_HOLDING:
        if (rx_len == LEN_REQUEST) {
            handle_read_holding();
        }
        break;
    case FN_WRITE_SINGLE:
        if (rx_len == LEN_REQUEST) {
            handle_write_single();
        }
        break;
    default:
        send_exception(rx_buf[1], EX_ILLEGAL_FUNCTION);
        break;
    }
}


/* ---- chip UID ---------------------------------------------------------- */

/* Big-endian: same order minichlink prints the UID in and the master sends it. */
static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void uid_init(void)
{
    my_uid = be64((const uint8_t *)0x1FFFF7E8);
}


/* ---- address storage in flash ------------------------------------------ */

/* Flash erases and writes in 64-byte pages, so the address gets the last
 * page to itself. Record layout (32-bit words):
 *   0: magic    1: address    2: address inverted (corruption check)
 * Reflashing the firmware doesn't touch this page. */
#define ID_PAGE_ADDR  0x08003FC0UL
#define ID_MAGIC      0x4D4F4442UL

#define FL_BSY      (1u << 0)
#define FL_STRT     (1u << 6)
#define FL_LOCK     (1u << 7)
#define FL_FLOCK    (1u << 15)
#define FL_FTPG     (1u << 16)     /* fast page program */
#define FL_FTER     (1u << 17)     /* fast page erase */
#define FL_BUFLOAD  (1u << 18)
#define FL_BUFRST   (1u << 19)

static void flash_wait(void)
{
    while (FLASH->STATR & FL_BSY);
}

/* id_save(0) clears the stored address. Blocks for a few ms and the chip
 * can't receive in that time. */
static void id_save(uint8_t id)
{
    uint32_t page[16];
    for (int i = 0; i < 16; i++) page[i] = 0xFFFFFFFF;
    page[0] = ID_MAGIC;
    page[1] = id;
    page[2] = ~(uint32_t)id;

    /* unlock sequence from the reference manual */
    FLASH->KEYR     = 0x45670123;
    FLASH->KEYR     = 0xCDEF89AB;
    FLASH->MODEKEYR = 0x45670123;
    FLASH->MODEKEYR = 0xCDEF89AB;

    FLASH->CTLR = FL_FTER;
    FLASH->ADDR = ID_PAGE_ADDR;
    FLASH->CTLR |= FL_STRT;
    flash_wait();
    FLASH->CTLR = 0;

    FLASH->CTLR = FL_FTPG;
    FLASH->CTLR |= FL_BUFRST;
    flash_wait();
    for (int i = 0; i < 16; i++) {
        *(volatile uint32_t *)(ID_PAGE_ADDR + 4 * i) = page[i];
        FLASH->CTLR |= FL_BUFLOAD;
        flash_wait();
    }
    FLASH->ADDR = ID_PAGE_ADDR;
    FLASH->CTLR |= FL_STRT;
    flash_wait();
    FLASH->CTLR = 0;

    FLASH->CTLR = FL_LOCK | FL_FLOCK;
}

/* Returns 0 if nothing valid is stored. */
static uint8_t id_load(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)ID_PAGE_ADDR;
    uint32_t id = p[1];
    if (p[0] == ID_MAGIC && p[2] == ~id && id >= 1 && id <= ID_MAX) {
        return (uint8_t)id;
    }
    return 0;
}


/* ---- discovery --------------------------------------------------------- */

static void handle_broadcast(void)
{
    switch (rx_buf[1]) {
    case FN_COMPARE:
        if (rx_len == LEN_COMPARE && slave_id == ID_BROADCAST && my_uid <= be64(&rx_buf[2])) {
            uint8_t b = REPLY_BYTE;
            tx_frame(&b, 1);
        }
        break;
#if DEFAULT_ID == 0
    /* Fixed-ID builds don't take part in address assignment. */
    case FN_ASSIGN:
        /* No reply. The master confirms by reading reg 0 at the new address. */
        if (rx_len == LEN_ASSIGN && be64(&rx_buf[2]) == my_uid &&
            rx_buf[10] >= 1 && rx_buf[10] <= ID_MAX) {
            slave_id = rx_buf[10];
            id_save(slave_id);
        }
        break;
    case FN_FORGET:
        if (rx_len == LEN_FORGET && be64(&rx_buf[2]) == my_uid) {
            slave_id = DEFAULT_ID;
            id_save(0);
        }
        break;
#endif
    default:
        break;
    }
}


/*
 * Shared modules for FID_SWITCH and FID_PLUG.
 * Implements software switch debouncing and auto-timeout LED control.
 */
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
    
    /* Invert raw reading: active-low input */
    sw_raw_last = sw_stable = !funDigitalRead(SW_PIN);
}

/**
 * @brief Polls switch state with non-blocking debounce filter.
 * @return 1 on state edge transition, 0 otherwise.
 */
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

/**
 * @brief Polls LED register state and handles safety auto-off timeout.
 */
static void led_poll(void)
{
    static uint16_t last;
    static uint32_t on_since;

    if (regs[REG_LED] != last) {
        last = regs[REG_LED];
        funDigitalWrite(LED_PIN, last ? LED_ACTIVE : !LED_ACTIVE);
        on_since = SysTick->CNT;
    }
    /* Hardware watchdog fallback if master abandons ON state */
    else if (last && (uint32_t)(SysTick->CNT - on_since) > (LED_TIMEOUT_MS * DELAY_MS_TIME)) {
        regs[REG_LED] = 0;
    }
}

#endif /* FLAVOR == FID_SWITCH || FLAVOR == FID_PLUG */


/* ---- flavors ----------------------------------------------------------- */
/* Each flavor provides flavor_init() (pins, peripherals, register setup)
 * and flavor_poll() (called every main loop pass, syncs regs and hardware). */

#if FLAVOR == FID_SWITCH
/* reg 1: switch state, 1 = on (read-only)
 * reg 2: identification LED (read/write), only the commissioning script uses it */
#define REG_SW_STATE  1

static void flavor_init(void)
{
    sw_init();
    led_init();
    regs[REG_SW_STATE] = sw_stable;
}

static void flavor_poll(void)
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

static void flavor_init(void)
{
    funPinMode(LIGHT_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(LIGHT_PIN, !LIGHT_ACTIVE);
    regs[REG_ONOFF] = 0;
    reg_max[REG_ONOFF] = 1;
}

static void flavor_poll(void)
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

static void flavor_init(void)
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

static void flavor_poll(void)
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

static void flavor_init(void)
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

static void flavor_poll(void)
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

/*
 * Register Map:
 *   REG_ONOFF (1): Relay state [0 = Open/OFF, 1 = Closed/ON]
 *   REG_LED   (2): Ident LED (handled in shared module)
 */
#define RELAY_PIN     PD2
#define RELAY_ACTIVE  1
#define REG_ONOFF     1

static uint16_t last_state = 0xFFFF;

static void flavor_init(void)
{
    /* Initialize relay in safe/off state */
    funPinMode(RELAY_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(RELAY_PIN, !RELAY_ACTIVE);

    regs[REG_ONOFF] = 0;
    reg_max[REG_ONOFF] = 1;

    sw_init();
    led_init();
}

static void flavor_poll(void)
{
    /* Toggle state on button press edge */
    if (sw_poll() && sw_stable) {
        regs[REG_ONOFF] = !regs[REG_ONOFF];
    }

    /* Synchronize physical relay output with register value */
    if (regs[REG_ONOFF] != last_state) {
        last_state = regs[REG_ONOFF];
        funDigitalWrite(RELAY_PIN, last_state ? RELAY_ACTIVE : !RELAY_ACTIVE);
        debugln("relay %s", last_state ? "ON" : "OFF");
    }

    led_poll();
}
#else
/* FID_NONE or not implemented yet */
static void flavor_init(void) {}
static void flavor_poll(void) {}
#endif


/* ---- main -------------------------------------------------------------- */

int main(void)
{
    SystemInit();
    debug_init();
    uart_init();
    uid_init();
#if DEFAULT_ID == 0
    slave_id = id_load();
#endif
    flavor_init();
    debugln("flavor %d, id %d, uid %08X%08X", FLAVOR, slave_id,
            (unsigned)(my_uid >> 32), (unsigned)my_uid);

    while (1) {
        flavor_poll();

        if (frame_poll()) {
            if (rx_overflow) {
                debugln("overflow, dropped");
            } else if (rx_len < 4) {
                debugln("too short (%d bytes)", rx_len);
            } else if (!crc_good()) {
                debugln("bad CRC");
            } else if (rx_buf[0] == ID_BROADCAST) {
                handle_broadcast();         /* no logging here, debugln is slow */
            } else if (rx_buf[0] != slave_id) {
                debugln("not for me (id %d)", rx_buf[0]);
            } else {
                handle_frame();             /* reply first, log after */
                debugln("handled func %02X", rx_buf[1]);
            }

            rx_len = 0;
            rx_overflow = 0;
        }
    }
}