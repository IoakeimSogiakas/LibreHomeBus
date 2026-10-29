/*
 * TPS92520-Q1 dual buck LED driver over SPI1.
 *
 * Frame (16 bit, MSB first, mode 0):  CMD | A5..A0 | PAR | D7..D0
 *   CMD 1 = write, PAR makes the number of 1s odd.
 * The reply to a frame comes back during the next one.
 */

#include "tps92520.h"

#if FLAVOR == FID_TW_SMART

#define CS_PIN     PC4
#define SCK_PIN    PC5
#define MOSI_PIN   PC6
#define MISO_PIN   PC7      /* open drain on the TPS side, needs a pull-up */

/* 48 MHz / 64 = 750 kHz. Datasheet max is 2 MHz; MISO rises through the
 * pull-up, so leave margin. */
#define SPI_DIV    SPI_BaudRatePrescaler_64

#define RD_MASK    0xFC00   /* SPE + the fixed 11000 field */
#define RD_OK      0x6000

void tps_spi_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_SPI1;

    funPinMode(CS_PIN, GPIO_CFGLR_OUT_10Mhz_PP);
    funDigitalWrite(CS_PIN, FUN_HIGH);
    funPinMode(SCK_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);
    funPinMode(MOSI_PIN, GPIO_CFGLR_OUT_10Mhz_AF_PP);
    funPinMode(MISO_PIN, GPIO_CFGLR_IN_PUPD);
    funDigitalWrite(MISO_PIN, FUN_HIGH);

    SPI1->CTLR1 = SPI_Direction_2Lines_FullDuplex | SPI_Mode_Master |
                  SPI_DataSize_16b | SPI_CPOL_Low | SPI_CPHA_1Edge |
                  SPI_NSS_Soft | SPI_DIV | SPI_FirstBit_MSB;
    SPI1->CTLR1 |= CTLR1_SPE_Set;
}

static uint16_t xfer(uint16_t out)
{
    funDigitalWrite(CS_PIN, FUN_LOW);
    Delay_Us(1);
    (void)SPI1->DATAR;
    while (!(SPI1->STATR & SPI_STATR_TXE));
    SPI1->DATAR = out;
    while (!(SPI1->STATR & SPI_STATR_RXNE));
    uint16_t in = SPI1->DATAR;
    while (SPI1->STATR & SPI_STATR_BSY);
    Delay_Us(1);
    funDigitalWrite(CS_PIN, FUN_HIGH);
    Delay_Us(2);
    return in;
}

static uint16_t frame(uint8_t wr, uint8_t addr, uint8_t data)
{
    uint16_t f = ((uint16_t)(wr ? 1 : 0) << 15) | ((uint16_t)(addr & 0x3F) << 9) | data;
    uint16_t p = f;
    p ^= p >> 8;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (!(p & 1)) f |= 1u << 8;
    return f;
}

int tps_read(uint8_t addr)
{
    xfer(frame(0, addr, 0));
    uint16_t r = xfer(frame(0, TPS_SYSCFG1, 0));
    if ((r & RD_MASK) != RD_OK) return -1;
    return r & 0xFF;
}

void tps_write(uint8_t addr, uint8_t data)
{
    xfer(frame(1, addr, data));
}

/* Write, then check the echo the TPS sends back in the next frame. */
static int write_check(uint8_t addr, uint8_t data)
{
    uint16_t f = frame(1, addr, data);
    xfer(f);
    uint16_t r = xfer(frame(0, TPS_SYSCFG1, 0));
    uint16_t want = ((f >> 1) & 0x7F00) | data;
    return r == want;
}

static int write_iadj(uint8_t reg_l, uint16_t v)
{
    return write_check(reg_l + 1, v >> 2) && write_check(reg_l, v & 3);
}

int tps_init(uint16_t iadj1, uint16_t iadj2, uint8_t ton, uint8_t pwmdiv)
{
    const uint8_t run = TPS_CMWEN | TPS_CH1INTPWM | TPS_CH1EN | TPS_CH2INTPWM | TPS_CH2EN;

    int s3 = tps_read(TPS_STATUS3);
    if (s3 < 0) {
        s3 = tps_read(TPS_STATUS3);     /* TPS may still be coming out of POR */
        if (s3 < 0) return 0;
    }
    if (s3 & TPS_STANDALONE) {
        tps_write(TPS_RESET, 0xC3);
        if (tps_read(TPS_STATUS3) < 0) return 0;
    }

    if (iadj1 > 1023) iadj1 = 1023;
    if (iadj2 > 1023) iadj2 = 1023;

    return write_check(TPS_SYSCFG1, TPS_CMWEN) &&
           write_check(TPS_SYSCFG2, 0) &&
           write_check(TPS_PWMDIV, pwmdiv) &&
           write_check(TPS_CH1TON, ton) &&
           write_check(TPS_CH2TON, ton) &&
           write_iadj(TPS_CH1IADJL, iadj1) &&
           write_iadj(TPS_CH2IADJL, iadj2) &&
           write_check(TPS_CH1PWMH, 0) && write_check(TPS_CH1PWML, 0) &&
           write_check(TPS_CH2PWMH, 0) && write_check(TPS_CH2PWML, 0) &&
           write_check(TPS_LHCFG1, 0) &&
           write_check(TPS_SYSCFG1, run) &&
           (tps_write(TPS_SYSCFG1, run | TPS_FPINRST), 1);
}

#endif /* FLAVOR == FID_TW_SMART */