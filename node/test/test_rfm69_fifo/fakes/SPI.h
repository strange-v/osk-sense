#pragma once

#include <Arduino.h>

#define SPI_HAS_TRANSACTION
#define SPI_MODE0 0

struct SPISettings {
    SPISettings() = default;
    SPISettings(uint32_t, uint8_t, uint8_t) {
    }
};

// Implemented by the test as an SX1231 register file and FIFO.
class SPIClass {
  public:
    void begin();
    void beginTransaction(SPISettings settings);
    void endTransaction();
    uint8_t transfer(uint8_t value);
};

extern SPIClass SPI;
