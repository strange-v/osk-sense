#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t byte;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define RISING 3
#define MSBFIRST 1
#define SS 10

// Arduino's abs() is a macro, which returns an unsigned argument unchanged.
inline uint32_t abs(uint32_t value) {
    return value;
}

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void digitalWrite(uint8_t pin, uint8_t value);
void pinMode(uint8_t pin, uint8_t mode);
void attachInterrupt(uint8_t interrupt, void (*handler)(), int mode);
void detachInterrupt(uint8_t interrupt);
void noInterrupts();
void interrupts();
