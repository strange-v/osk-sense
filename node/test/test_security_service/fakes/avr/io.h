#pragma once
#include <stdint.h>
inline struct { uint8_t CTRLA = 0, STATUS = 0; } WDT;
inline struct { uint8_t SWRR = 0; } RSTCTRL;
#define WDT_SYNCBUSY_bm 1
#define WDT_PERIOD_8KCLK_gc 11
#define WDT_PERIOD_OFF_gc 0
#define RSTCTRL_SWRE_bm 1
#define _PROTECTED_WRITE(reg, value) ((reg) = (value))
