#pragma once

#include <stdint.h>

namespace radiosensors {
namespace protocol {

// Stable opaque keys: the numeric value does not encode capabilities,
// hardware, or Home Assistant presentation.
enum class ProfileId : uint16_t {
    Voltage = 1,
    Temperature = 2,
    ClimateTh = 3,
    ClimateThp = 4,
    Binary = 5,
    PulseCounter = 6,
    BinaryClimateTh = 7,
    BinaryTemperature = 8,
};

constexpr uint16_t profileIdValue(const ProfileId profile) {
    return static_cast<uint16_t>(profile);
}

} // namespace protocol
} // namespace radiosensors
