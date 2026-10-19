#pragma once
#include "ch32fun.h"
#include <stdio.h>

// --- Debug Logic ---
#define DEBUG_ENABLED 1
#if DEBUG_ENABLED
    #define debug_init()  do {} while(0)
    #define debug(...)    printf(__VA_ARGS__)
    #define debugln(...)  do { printf(__VA_ARGS__); printf("\n"); } while(0)
#else
    #define debug_init()  do {} while(0)
    #define debug(...)    do {} while(0)
    #define debugln(...)  do {} while(0)
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
