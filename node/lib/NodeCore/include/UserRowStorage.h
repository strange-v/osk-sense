#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace radiosensors {
namespace node {
namespace storage {

class UserRowStorage {
  public:
    uint8_t read(const size_t address) const {
        return *reinterpret_cast<volatile const uint8_t *>(USER_SIGNATURES_START + address);
    }
};

} // namespace storage
} // namespace node
} // namespace radiosensors
