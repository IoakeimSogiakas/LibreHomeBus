#pragma once
#include "config.h"

#define TPS_SYSCFG1   0x00
#define TPS_SYSCFG2   0x01
#define TPS_STATUS1   0x03
#define TPS_STATUS2   0x04
#define TPS_STATUS3   0x05
#define TPS_CH1IADJL  0x08
#define TPS_CH1IADJH  0x09
#define TPS_CH2IADJL  0x0A
#define TPS_CH2IADJH  0x0B
#define TPS_PWMDIV    0x0C
#define TPS_CH1PWML   0x0D
#define TPS_CH1PWMH   0x0E
#define TPS_CH2PWML   0x0F
#define TPS_CH2PWMH   0x10
#define TPS_CH1TON    0x11
#define TPS_CH2TON    0x12
#define TPS_LHCFG1    0x1E
#define TPS_RESET     0x2E

/* SYSCFG1 */
#define TPS_CH1EN     (1u << 0)
#define TPS_CH1INTPWM (1u << 1)
#define TPS_CH2EN     (1u << 2)
#define TPS_CH2INTPWM (1u << 3)
#define TPS_CMWEN     (1u << 4)
#define TPS_LHSW      (1u << 5)
#define TPS_FPINRST   (1u << 7)

/* STATUS3 */
#define TPS_CH1STATUS (1u << 0)
#define TPS_CH2STATUS (1u << 1)
#define TPS_PC        (1u << 2)
#define TPS_TW        (1u << 3)
#define TPS_CMWTO     (3u << 4)
#define TPS_STANDALONE (1u << 7)

#define TPS_PWM_MAX   1023

void tps_spi_init(void);
int  tps_read(uint8_t addr);                   /* -1 on SPI error */
void tps_write(uint8_t addr, uint8_t data);
int  tps_init(uint16_t iadj1, uint16_t iadj2, uint8_t ton, uint8_t pwmdiv);
