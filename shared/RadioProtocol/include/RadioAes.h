#pragma once

#include <stdint.h>

namespace radiosensors {
namespace radio_aes {

constexpr uint8_t kKeySize = 16;

// RFM69 register map values; the callers check them against RFM69registers.h.
constexpr uint8_t kStandbyMode = 1;
constexpr uint8_t kPacketConfig2Register = 0x3D;
constexpr uint8_t kAesKey1Register = 0x3E;
constexpr uint8_t kAesOn = 0x01;

// Loads a raw key and turns AES on. RFM69::encrypt() is not usable: it takes
// the key as a C string and leaves AES off when the first byte is zero, which
// a random key has one time in 256.
template <typename Radio> void enable(Radio &radio, const uint8_t (&key)[kKeySize]) {
    radio.setMode(kStandbyMode);
    for (uint8_t i = 0; i < kKeySize; ++i)
        radio.writeReg(static_cast<uint8_t>(kAesKey1Register + i), key[i]);
    radio.writeReg(kPacketConfig2Register,
                   static_cast<uint8_t>(radio.readReg(kPacketConfig2Register) | kAesOn));
}

} // namespace radio_aes
} // namespace radiosensors
