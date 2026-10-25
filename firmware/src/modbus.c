/*
 * Modbus RTU basics: UART, framing, CRC, and the normal read/write requests.
 * Discovery frames live in discovery.c.
 */

#include "node.h"


/* ---- protocol ---------------------------------------------------------- */

#define FN_READ_HOLDING   0x03
#define FN_WRITE_SINGLE   0x06

#define EX_ILLEGAL_FUNCTION  0x01
#define EX_ILLEGAL_ADDRESS   0x02
#define EX_ILLEGAL_VALUE     0x03

/* Exact length of a read or write request: address + function + 4 bytes + 2 CRC. */
#define LEN_REQUEST       8


/* ---- hardware ---------------------------------------------------------- */

#define BAUD         115200
#define DE_PIN       PD4         /* DE and /RE tied together: high = send */

/* A frame ends after 3.5 character times of silence. The spec fixes it at
 * 1750 us for anything above 19200 baud. */
#define T35_US       ((BAUD > 19200) ? 1750 : (38500000UL / BAUD))
#define T35_TICKS    (T35_US * DELAY_US_TIME)


/* ---- state ------------------------------------------------------------- */

/* Flavor code keeps regs in sync with the hardware. */
uint16_t regs[NUM_REGS] = { FLAVOR };

/* Highest value a write may set. 0 = read-only. */
uint16_t reg_max[NUM_REGS];

uint8_t  rx_buf[RX_BUF_SIZE];
uint8_t  rx_len;
uint8_t  rx_overflow;
uint8_t  slave_id = DEFAULT_ID;

static uint32_t rx_last;               /* SysTick value at the last byte */


/* ---- UART and framing -------------------------------------------------- */

void uart_init(void)
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
int frame_poll(void)
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

void tx_frame(const uint8_t *p, uint8_t n)
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
int crc_good(void)
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

void handle_frame(void)
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
