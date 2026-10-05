#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t userRow[32], serialNumber[10];
extern uint32_t fakeMillis;
#define USER_SIGNATURES_START reinterpret_cast<uintptr_t>(userRow)
#define SIGROW_SERNUM0 serialNumber[0]
#define PIN_SPI_MISO 1
#define INPUT_PULLUP 2
inline uint32_t millis() { return fakeMillis++; }
inline void pinMode(uint8_t,uint8_t) {}
