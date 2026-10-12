#pragma once

#include <EEPROM.h>
#include <stddef.h>
#include <stdint.h>

namespace radiosensors {
namespace node {
namespace storage {

class ArduinoEepromStorage {
  public:
    uint8_t read(const size_t address) const {
        return EEPROM.read(static_cast<uint8_t>(address));
    }

    void update(const size_t address, const uint8_t value) {
        EEPROM.update(static_cast<uint8_t>(address), value);
    }
};

} // namespace storage
} // namespace node
} // namespace radiosensors
