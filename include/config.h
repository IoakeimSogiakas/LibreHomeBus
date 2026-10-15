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