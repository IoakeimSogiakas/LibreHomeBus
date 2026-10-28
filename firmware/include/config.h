#pragma once
#include "ch32fun.h"
#include <stdio.h>

// --- Debug Logic ---
#define DEBUG_ENABLED 1

/*
 * Independent watchdog (IWDG).
 * 0 = disabled for bench/debug sessions (no automatic watchdog resets).
 * 1 = enabled for unattended/normal operation.
 * Can also be overridden with -DWATCHDOG_ENABLED=1 in build flags.
 */
#ifndef WATCHDOG_ENABLED
#define WATCHDOG_ENABLED 0
#endif
#if DEBUG_ENABLED
    #define debug_init()  do {} while(0)
    #define debug(...)    printf(__VA_ARGS__)
    #define debugln(...)  do { printf(__VA_ARGS__); printf("\n"); } while(0)
#else
    #define debug_init()  do {} while(0)
    #define debug(...)    do {} while(0)
    #define debugln(...)  do {} while(0)
#endif


#if WATCHDOG_ENABLED
/*
 * CH32V003 independent watchdog.
 * LSI is nominally 128 kHz; prescaler /64 and a 0x0FFF reload give
 * approximately 2.05 seconds between watchdog resets.
 */
static void watchdog_init(void)
{
    IWDG->CTLR = 0x5555;               /* Unlock prescaler/reload registers */
    IWDG->PSCR = 4;                    /* LSI / 64 */
    while (IWDG->STATR & IWDG_PVU) {}  /* Wait for prescaler update */

    IWDG->RLDR = 0x0FFF;               /* Maximum 12-bit reload value */
    while (IWDG->STATR & IWDG_RVU) {}  /* Wait for reload update */

    IWDG->CTLR = 0xAAAA;               /* Load the counter */
    IWDG->CTLR = 0xCCCC;               /* Start; cannot be stopped until reset */
}

static inline void watchdog_feed(void)
{
    IWDG->CTLR = 0xAAAA;
}
#else
#define watchdog_init() do {} while (0)
#define watchdog_feed() do {} while (0)
#endif


#define DEBUG_BUS 0     /* 1 = log every frame on the bus, slows the node down */
#if DEBUG_BUS
    #define busln(...)  debugln(__VA_ARGS__)
#else
    #define busln(...)  do {} while(0)
#endif

#ifndef DEFAULT_ID
#define DEFAULT_ID        0
#endif

#define FID_NONE         0
#define FID_SWITCH       1
#define FID_LIGHT_ONOFF  2
#define FID_LIGHT_DIM    3
#define FID_TW_SMART     4   /* tunable white, SPI driver (not done yet) */
#define FID_TW_PWM       5
#define FID_PLUG         6

#ifndef FLAVOR
#define FLAVOR FID_NONE
#endif
