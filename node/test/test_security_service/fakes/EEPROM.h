#pragma once
#include <stdint.h>
struct FakeEeprom {
    uint8_t data[256]; int remaining = -1;
    uint8_t read(uint8_t i) const { return data[i]; }
    void update(uint8_t i,uint8_t value) {
        if (remaining == 0) return;
        if (remaining > 0) --remaining;
        data[i] = value;
    }
};
extern FakeEeprom EEPROM;
