/*
 * CH32V003 Modbus RTU slave over RS-485, with automatic address discovery
 * -----------------------------------------------------------------------
 * One firmware, several device types ("flavors"). The flavor is chosen at
 * build time, and every chip tells the master which flavor it is by
 * reporting its flavor ID (FID) in register 0.
 *
 * BUILD FLAGS (platformio.ini -> build_flags)
 *   -DFLAVOR=<n>      which device type to build (see "Flavor IDs" below)
 *   -DDEFAULT_ID=<n>  fixed Modbus address 1..247, or 0 (default) for
 *                     "unassigned": the chip stays quiet on normal traffic,
 *                     takes part in discovery, and remembers the address it
 *                     is given in flash.
 *
 * REGISTER MAP (holding registers, read with function 03, write with 06)
 *   reg 0  FID, read-only (the same on every flavor)
 *   reg 1+ depends on the flavor, see the flavor blocks at the bottom
 *
 * DISCOVERY (broadcast address 0, custom function codes, no Modbus replies)
 *   00 41 <UID:8> CRC        "Is there an unassigned chip with UID <= this?"
 *                            Those chips answer with one byte (0x55).
 *                            The master halves the range until it has
 *                            found the lowest UID.
 *   00 42 <UID:8> <addr> CRC Give the chip with this UID the address <addr>.
 *                            The chip saves it to flash.
 *   00 43 <UID:8> CRC        Make the chip with this UID forget its address.
 *
 * Needs the ch32fun framework (config.h pulls it in).
 */

#include "config.h"


/* ====================================================================== */
/*  Protocol constants                                                     */
/* ====================================================================== */

/* Function codes are the second byte of every Modbus message: they say what
 * the message wants. 0x03 and 0x06 are standard Modbus. Modbus sets aside
 * 65-72 (0x41-0x48) and 100-110 for custom use, so ours cannot clash with
 * any standard function. */
#define FN_READ_HOLDING   0x03   /* standard: read registers                */
#define FN_WRITE_SINGLE   0x06   /* standard: write one register            */
#define FN_COMPARE        0x41   /* custom: "anyone with UID <= X?"         */
#define FN_ASSIGN         0x42   /* custom: give the chip with this UID an address */
#define FN_FORGET         0x43   /* custom: erase the chip's saved address  */

/* Exception codes: the reason a request was refused. */
#define EX_ILLEGAL_FUNCTION  0x01
#define EX_ILLEGAL_ADDRESS   0x02
#define EX_ILLEGAL_VALUE     0x03

/* The single byte an unassigned chip sends to say "yes" during discovery.
 * 0x55 = binary 01010101. On the wire that is a bit pattern that flips every
 * bit, which is easy to recognise on a scope or logic analyser. The master
 * only checks "did any byte arrive", so the exact value is not important. */
#define REPLY_BYTE        0x55

/* Exact message lengths in bytes: address + function + payload + 2 CRC. */
#define LEN_REQUEST       8      /* standard read/write request             */
#define LEN_COMPARE       12     /* 00 41 <UID:8> CRC                       */
#define LEN_ASSIGN        13     /* 00 42 <UID:8> <addr> CRC                */
#define LEN_FORGET        12     /* 00 43 <UID:8> CRC                       */

#define ID_BROADCAST      0      /* address 0 = everyone; also "unassigned" */
#define ID_MAX            247    /* highest valid Modbus slave address      */

#ifndef DEFAULT_ID
#define DEFAULT_ID        0      /* 0 = unassigned (see header comment)     */
#endif


/* ====================================================================== */
/*  Hardware settings                                                      */
/* ====================================================================== */

#define BAUD         115200      /* serial speed, must match the master     */
#define RX_BUF_SIZE  64          /* longest message we can hold             */
#define DE_PIN       PD4         /* wired to the transceiver's DE and /RE   */
                                 /* high = transmit, low = listen           */

/* A message ends after 3.5 character times of silence. Above 19200 baud
 * the Modbus standard fixes this at 1750 us. */
#define T35_US       ((BAUD > 19200) ? 1750 : (38500000UL / BAUD))
#define T35_TICKS    (T35_US * DELAY_US_TIME)   /* same time in timer ticks */


/* ====================================================================== */
/*  Flavor IDs                                                             */
/* ====================================================================== */

#define FID_NONE         0   /* no valid identity                           */
#define FID_SWITCH       1   /* toggle switch input                         */
#define FID_LIGHT_ONOFF  2   /* light, on/off only                          */
#define FID_LIGHT_DIM    3   /* light, dimmable (PWM)                       */
#define FID_TW_SMART     4   /* tunable white, SPI driver                   */
#define FID_TW_PWM       5   /* tunable white, PWM into a plain driver      */

#ifndef FLAVOR
#define FLAVOR FID_NONE      /* an unflavored build reports FID 0 and does nothing */
#endif


/* ====================================================================== */
/*  Register table                                                         */
/* ====================================================================== */

#define NUM_REGS     4

/* regs[] is the "notice board" the master reads. The Modbus code only reads
 * and writes this table; the flavor code at the bottom keeps it up to date. */
static uint16_t regs[NUM_REGS] = { FLAVOR };    /* reg 0 = FID, the rest 0 */

/* Highest value a register accepts from a write. 0 means read-only, so
 * everything is read-only until a flavor makes a register writable. */
static uint16_t reg_max[NUM_REGS];


/* ====================================================================== */
/*  Receive state                                                          */
/* ====================================================================== */

static uint8_t  rx_buf[RX_BUF_SIZE];   /* bytes of the message being received */
static uint8_t  rx_len;                /* how many bytes are in rx_buf        */
static uint8_t  rx_overflow;           /* 1 = message too long, throw it away */
static uint32_t rx_last;               /* SysTick time the last byte arrived  */

static uint8_t  slave_id = DEFAULT_ID; /* our address; 0 = unassigned         */
static uint64_t my_uid;                /* factory-unique chip ID              */


/* ====================================================================== */
/*  UART and message framing                                               */
/* ====================================================================== */

static void uart_init(void)
{
    funGpioInitAll();
    RCC->APB2PCENR |= RCC_APB2Periph_USART1;

    funPinMode(PD5, GPIO_CFGLR_OUT_10Mhz_AF_PP);   /* TX                    */
    funPinMode(PD6, GPIO_CFGLR_IN_FLOAT);          /* RX                    */
    funPinMode(DE_PIN, GPIO_CFGLR_OUT_10Mhz_PP);   /* DE and /RE together   */
    funDigitalWrite(DE_PIN, FUN_LOW);              /* low = listen mode     */

    USART1->BRR   = (FUNCONF_SYSTEM_CORE_CLOCK + BAUD / 2) / BAUD;
    USART1->CTLR1 = USART_WordLength_8b | USART_Parity_No | USART_Mode_Tx | USART_Mode_Rx;
    USART1->CTLR2 = USART_StopBits_1;
    USART1->CTLR3 = USART_HardwareFlowControl_None;
    USART1->CTLR1 |= CTLR1_UE_Set;                 /* switch the UART on    */
}

/* Collects incoming bytes. Returns 1 when a complete message is waiting
 * in rx_buf. Call it as often as possible from the main loop. */
static int frame_poll(void)
{
    if (USART1->STATR & USART_FLAG_RXNE) {
        uint8_t b = USART1->DATAR;                 /* reading DATAR clears RXNE */
        rx_last = SysTick->CNT;
        if (rx_len < RX_BUF_SIZE) {
            rx_buf[rx_len++] = b;
        } else {
            rx_overflow = 1;
        }
        /* The discovery question has a fixed length, so answer as soon as
         * it is complete instead of waiting out the silence. This keeps
         * the 64 questions of a search fast. */
        if (rx_len == LEN_COMPARE && rx_buf[0] == ID_BROADCAST && rx_buf[1] == FN_COMPARE) {
            return 1;
        }
    } else if (rx_len && (uint32_t)(SysTick->CNT - rx_last) > T35_TICKS) {
        return 1;                                  /* line went quiet: message over */
    }
    return 0;
}

static void tx_frame(const uint8_t *p, uint8_t n)
{
    funDigitalWrite(DE_PIN, FUN_HIGH);              /* transmit mode        */

    for (uint8_t i = 0; i < n; i++) {
        while (!(USART1->STATR & USART_FLAG_TXE));  /* wait for a free TX slot */
        USART1->DATAR = p[i];
    }

    while (!(USART1->STATR & USART_FLAG_TC));       /* wait until the last bit is out */
    funDigitalWrite(DE_PIN, FUN_LOW);               /* back to listening    */
}


/* ====================================================================== */
/*  CRC-16 (Modbus)                                                        */
/* ====================================================================== */

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

/* Checks the CRC of the message currently in rx_buf. */
static int crc_good(void)
{
    uint16_t got = rx_buf[rx_len - 2] | (rx_buf[rx_len - 1] << 8);  /* CRC arrives low byte first */
    return got == crc16(rx_buf, rx_len - 2);
}


/* ====================================================================== */
/*  Normal Modbus requests (addressed to us)                               */
/* ====================================================================== */

static void send_exception(uint8_t func, uint8_t code)
{
    uint8_t r[5] = { slave_id, (uint8_t)(func | 0x80), code, 0, 0 };   /* 0x80 flags an error reply */
    uint16_t c = crc16(r, 3);
    r[3] = c & 0xFF;
    r[4] = c >> 8;
    tx_frame(r, 5);
}

/* Function 03: read one or more registers. */
static void handle_read_holding(void)
{
    uint16_t addr  = (rx_buf[2] << 8) | rx_buf[3];   /* first register    */
    uint16_t count = (rx_buf[4] << 8) | rx_buf[5];   /* how many          */

    if (count < 1 || count > 125) {
        send_exception(FN_READ_HOLDING, EX_ILLEGAL_VALUE);
        return;
    }
    if (addr + count > NUM_REGS) {
        send_exception(FN_READ_HOLDING, EX_ILLEGAL_ADDRESS);
        return;
    }

    /* Header (3 bytes) + register data (2 per register) + CRC (2 bytes) */
    uint8_t r[3 + 2 * NUM_REGS + 2];
    uint8_t n = 0;

    r[n++] = slave_id;
    r[n++] = FN_READ_HOLDING;
    r[n++] = count * 2;                              /* byte count        */

    for (uint16_t i = 0; i < count; i++) {
        r[n++] = regs[addr + i] >> 8;                /* high byte first   */
        r[n++] = regs[addr + i] & 0xFF;
    }

    uint16_t c = crc16(r, n);
    r[n++] = c & 0xFF;                               /* CRC low byte first */
    r[n++] = c >> 8;
    tx_frame(r, n);
}

/* Function 06: write one register. */
static void handle_write_single(void)
{
    uint16_t addr  = (rx_buf[2] << 8) | rx_buf[3];
    uint16_t value = (rx_buf[4] << 8) | rx_buf[5];

    if (addr >= NUM_REGS || reg_max[addr] == 0) {    /* missing or read-only */
        send_exception(FN_WRITE_SINGLE, EX_ILLEGAL_ADDRESS);
        return;
    }
    if (value > reg_max[addr]) {
        send_exception(FN_WRITE_SINGLE, EX_ILLEGAL_VALUE);
        return;
    }
    regs[addr] = value;
    tx_frame(rx_buf, LEN_REQUEST);                   /* Modbus confirms a write by echoing the request */
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


/* ====================================================================== */
/*  Chip UID                                                               */
/* ====================================================================== */

/* Reads 8 bytes as one number, first byte = most significant. This is the
 * order minichlink prints the UID in, and the order the master sends it. */
static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void uid_init(void)
{
    my_uid = be64((const uint8_t *)0x1FFFF7E8);   /* factory-programmed ID area */
}


/* ====================================================================== */
/*  Remembering the address in flash                                       */
/* ====================================================================== */

/* Flash can only be erased and written in whole pages (64 bytes in fast
 * mode), so the address gets a page of its own: the last one, which the
 * program does not use. The record is three words:
 *   word 0: magic number (marks the page as ours)
 *   word 1: the address
 *   word 2: the address with every bit flipped (catches corrupt data)
 * Reflashing the firmware leaves this page alone, so the address survives
 * updates. */
#define ID_PAGE_ADDR  0x08003FC0UL     /* last 64-byte page of the 16 KB flash */
#define ID_MAGIC      0x4D4F4442UL

/* Flash control register bits (CH32V003) */
#define FL_BSY      (1u << 0)          /* STATR: flash is busy              */
#define FL_STRT     (1u << 6)          /* start the erase / write           */
#define FL_LOCK     (1u << 7)          /* normal lock                       */
#define FL_FLOCK    (1u << 15)         /* fast-mode lock                    */
#define FL_FTPG     (1u << 16)         /* fast page program mode            */
#define FL_FTER     (1u << 17)         /* fast page erase mode              */
#define FL_BUFLOAD  (1u << 18)         /* push one word into the page buffer */
#define FL_BUFRST   (1u << 19)         /* empty the page buffer             */

static void flash_wait(void)
{
    while (FLASH->STATR & FL_BSY);
}

/* Saves an address. id_save(0) erases it (0 is never accepted by id_load).
 * Takes a few milliseconds, during which the chip cannot receive. */
static void id_save(uint8_t id)
{
    uint32_t page[16];
    for (int i = 0; i < 16; i++) page[i] = 0xFFFFFFFF;   /* unused words stay blank */
    page[0] = ID_MAGIC;
    page[1] = id;
    page[2] = ~(uint32_t)id;

    /* Unlock: flash only listens after two key values are written. */
    FLASH->KEYR     = 0x45670123;
    FLASH->KEYR     = 0xCDEF89AB;
    FLASH->MODEKEYR = 0x45670123;
    FLASH->MODEKEYR = 0xCDEF89AB;

    /* 1. Erase the page. */
    FLASH->CTLR = FL_FTER;
    FLASH->ADDR = ID_PAGE_ADDR;
    FLASH->CTLR |= FL_STRT;
    flash_wait();
    FLASH->CTLR = 0;

    /* 2. Write the page: fill the buffer word by word, then commit it. */
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

    FLASH->CTLR = FL_LOCK | FL_FLOCK;                    /* lock it again   */
}

/* Returns the saved address, or 0 if nothing valid is stored. */
static uint8_t id_load(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)ID_PAGE_ADDR;
    uint32_t id = p[1];
    if (p[0] == ID_MAGIC && p[2] == ~id && id >= 1 && id <= ID_MAX) {
        return (uint8_t)id;
    }
    return 0;
}


/* ====================================================================== */
/*  Discovery (broadcast messages)                                         */
/* ====================================================================== */

static void handle_broadcast(void)
{
    switch (rx_buf[1]) {
    case FN_COMPARE:
        /* Only unassigned chips take part. Answer if our UID is at or
         * below the threshold the master sent. */
        if (rx_len == LEN_COMPARE && slave_id == ID_BROADCAST && my_uid <= be64(&rx_buf[2])) {
            uint8_t b = REPLY_BYTE;
            tx_frame(&b, 1);
        }
        break;
    case FN_ASSIGN:
        /* Bytes 2-9 = UID, byte 10 = the new address. No reply: the
         * master checks by reading register 0 at the new address. */
        if (rx_len == LEN_ASSIGN && be64(&rx_buf[2]) == my_uid &&
            rx_buf[10] >= 1 && rx_buf[10] <= ID_MAX) {
            slave_id = rx_buf[10];
            id_save(slave_id);                    /* keep it through power loss */
        }
        break;
    case FN_FORGET:
        if (rx_len == LEN_FORGET && be64(&rx_buf[2]) == my_uid) {
            slave_id = DEFAULT_ID;                /* back to unassigned */
            id_save(0);                           /* erase the saved copy */
        }
        break;
    default:
        break;                                    /* broadcasts never get replies */
    }
}


/* ====================================================================== */
/*  Flavors: one block per device type                                     */
/*                                                                         */
/*  Each flavor provides:                                                  */
/*    flavor_init()  set up pins/peripherals, declare its registers        */
/*    flavor_poll()  called every pass of the main loop; turns register    */
/*                   values into hardware actions (or the reverse)         */
/* ====================================================================== */

#if FLAVOR == FID_SWITCH
/* Toggle switch input.  reg 1: 1 = switch on, 0 = off (read-only) */
#define SW_PIN        PC1      /* switch between this pin and ground */
#define REG_SW_STATE  1
#define DEBOUNCE_MS   20       /* position must hold this long to be believed */

static uint8_t  sw_raw_last;   /* what the pin said on the previous pass */
static uint8_t  sw_stable;     /* the position we currently believe      */
static uint32_t sw_changed;    /* when the pin last moved                */

static void flavor_init(void)
{
    funPinMode(SW_PIN, GPIO_CFGLR_IN_PUPD);              /* input with pull resistor   */
    funDigitalWrite(SW_PIN, FUN_HIGH);                   /* in this mode, high = pull-up */
    sw_raw_last = sw_stable = !funDigitalRead(SW_PIN);   /* read the position at power-up */
    regs[REG_SW_STATE] = sw_stable;
}

static void flavor_poll(void)
{
    uint8_t raw = !funDigitalRead(SW_PIN);               /* closed switch pulls the pin low */
    if (raw != sw_raw_last) {                            /* pin moved: restart the timer */
        sw_raw_last = raw;
        sw_changed  = SysTick->CNT;
    }
    else if (raw != sw_stable &&
             (uint32_t)(SysTick->CNT - sw_changed) > DEBOUNCE_MS * DELAY_MS_TIME) {
        sw_stable = raw;                                 /* steady long enough: accept it */
        regs[REG_SW_STATE] = raw;
        debugln("switch %s", raw ? "ON" : "OFF");
    }
}

#elif FLAVOR == FID_LIGHT_ONOFF
/* Light with on/off only.  reg 1: 0 = off, 1 = on (read/write) */
#define LIGHT_PIN     PD2      /* output pin to the relay / driver            */
#define LIGHT_ACTIVE  1        /* 1 = pin high turns the light on, 0 = pin low */
#define REG_ONOFF     1

static uint16_t last_state = 0xFFFF;   /* impossible value, so the first pass always applies */

static void flavor_init(void)
{
    funPinMode(LIGHT_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(LIGHT_PIN, !LIGHT_ACTIVE);           /* start off          */
    regs[REG_ONOFF] = 0;
    reg_max[REG_ONOFF] = 1;                              /* writable: 0 or 1   */
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
/* Dimmable light, PWM on TIM1 channel 1.
 *   reg 1: 0 = off, 1 = on          (read/write)
 *   reg 2: brightness 0..1000       (read/write, 1000 = full) */
#define PWM_PIN       PD2
#define REG_ONOFF     1
#define REG_BRT       2
#define PWM_STEPS     1000

static uint16_t last_duty = 0xFFFF;    /* impossible value, so the first pass always applies */

static void flavor_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
    funPinMode(PWM_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);     /* hand the pin to the timer */

    TIM1->PSC   = 0;                    /* no prescaler: timer runs at the full 48 MHz */
    TIM1->ATRLR = PWM_STEPS - 1;        /* count 0..999, then restart: 48 MHz / 1000 = 48 kHz */
    TIM1->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;   /* PWM mode 1, buffered duty value */
    TIM1->CCER  |= TIM_CC1E;            /* channel 1 output on */
    TIM1->BDTR  |= TIM_MOE;             /* main output enable (TIM1 needs this extra switch) */
    TIM1->CH1CVR = 0;                   /* start dark */
    TIM1->SWEVGR |= TIM_UG;             /* load the settings */
    TIM1->CTLR1 |= TIM_CEN;             /* start counting */

    regs[REG_ONOFF] = 0;
    regs[REG_BRT]   = PWM_STEPS;
    reg_max[REG_ONOFF] = 1;             /* these two are writable */
    reg_max[REG_BRT]   = PWM_STEPS;
}

static void flavor_poll(void)
{
    uint16_t duty = regs[REG_ONOFF] ? regs[REG_BRT] : 0;
    if (duty != last_duty) {            /* only touch the timer when something changed */
        last_duty = duty;
        TIM1->CH1CVR = duty;
        debugln("duty %u", duty);
    }
}

#elif FLAVOR == FID_TW_PWM
/* Tunable white: two PWM channels, each into an LED driver's DIM pin.
 *   reg 1: 0 = off, 1 = on          (read/write)
 *   reg 2: cold channel 0..1000     (read/write)
 *   reg 3: warm channel 0..1000     (read/write)
 * The master (ESPHome) works out the mix; this chip only sets two brightnesses. */
#define PWM_COLD_PIN  PD2      /* TIM1 channel 1 */
#define PWM_WARM_PIN  PC3      /* TIM1 channel 3 */
#define REG_ONOFF     1
#define REG_COLD      2
#define REG_WARM      3
#define PWM_STEPS     1000
#define PWM_PSC       47       /* PWM frequency = 48 MHz / (PSC+1) / STEPS = 1 kHz */

static uint16_t last_cold = 0xFFFF;    /* impossible values, so the first pass always applies */
static uint16_t last_warm = 0xFFFF;

static void flavor_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_TIM1;
    funPinMode(PWM_COLD_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);   /* hand both pins to the timer */
    funPinMode(PWM_WARM_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);

    TIM1->PSC   = PWM_PSC;
    TIM1->ATRLR = PWM_STEPS - 1;
    TIM1->CHCTLR1 |= TIM_OC1M_2 | TIM_OC1M_1 | TIM_OC1PE;   /* channel 1: PWM mode 1, buffered */
    TIM1->CHCTLR2 |= TIM_OC3M_2 | TIM_OC3M_1 | TIM_OC3PE;   /* channel 3: same */
    TIM1->CCER  |= TIM_CC1E | TIM_CC3E;                     /* both outputs on */
    TIM1->BDTR  |= TIM_MOE;
    TIM1->CH1CVR = 0;                                       /* start dark */
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

#else
/* FID_NONE, or a flavor that is not implemented yet: do nothing. */
static void flavor_init(void) {}
static void flavor_poll(void) {}
#endif


/* ====================================================================== */
/*  Main                                                                   */
/* ====================================================================== */

int main(void)
{
    SystemInit();
    debug_init();
    uart_init();
    uid_init();
#if DEFAULT_ID == 0
    slave_id = id_load();      /* unassigned build: use the saved address if there is one */
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
                handle_broadcast();                 /* no print here: it's too slow */
            } else if (rx_buf[0] != slave_id) {
                debugln("not for me (id %d)", rx_buf[0]);
            } else {
                handle_frame();                     /* reply on the bus before logging */
                debugln("handled func %02X", rx_buf[1]);
            }

            rx_len = 0;
            rx_overflow = 0;
        }
    }
}