#pragma once
#define CH32V003 1                  // Targeting WCH CH32V003 series
#define FUNCONF_USE_PLL 1               // Use built-in 2x PLL 
#define FUNCONF_USE_HSI 1               // Use HSI Internal Oscillator
#define FUNCONF_SYSTEM_CORE_CLOCK 48000000  // 48 MHz default for CH32V003
#define FUNCONF_SYSTICK_USE_HCLK 0      // SysTick at HCLK/8 (6 MHz)
#define FUNCONF_USE_5V_VDD 1            // OK if you're running at 5V logic

// ===== Debug / printf configuration =====
#define FUNCONF_USE_DEBUGPRINTF 1       // ✅ Enable debug printf support       
#define FUNCONF_USE_UARTPRINTF  0       
#define FUNCONF_UART_PRINTF_BAUD 115200 // Only used if FUNCONF_USE_UARTPRINTF is set.
#define FUNCONF_NULL_PRINTF 0           // ✅ Make sure printf actually works
// #define FUNCONF_DEBUGPRINTF_TIMEOUT 0x80000 // ~120ms printf timeout (optional)
#define FUNCONF_DEBUG_HARDFAULT 1       // Optional: helpful crash reporting


