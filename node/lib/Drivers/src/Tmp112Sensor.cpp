#include "Tmp112Sensor.h"

#include "TwiBusRecovery.h"

namespace radiosensors {
namespace node {

void Tmp112Sensor::begin() {
    wire_.begin();
    enterShutdown();
}

void Tmp112Sensor::enterShutdown() {
    uint16_t configuration = 0;
    if (readRegister16(0x01, configuration)) {
        // SD=1 keeps the sensor in its sub-microamp shutdown state between
        // scheduled measurements.
        writeRegister16(0x01, configuration | 0x0100U);
    }
}

bool Tmp112Sensor::readRegister16(const uint8_t registerAddress, uint16_t &value) {
    wire_.beginTransmission(address_);
    wire_.write(registerAddress);
    if (wire_.endTransmission(false) != 0)
        return false;
    if (wire_.requestFrom(address_, static_cast<uint8_t>(2)) != 2)
        return false;
    value = static_cast<uint16_t>(wire_.read()) << 8;
    value |= static_cast<uint8_t>(wire_.read());
    return true;
}

bool Tmp112Sensor::writeRegister16(const uint8_t registerAddress, const uint16_t value) {
    wire_.beginTransmission(address_);
    wire_.write(registerAddress);
    wire_.write(static_cast<uint8_t>(value >> 8));
    wire_.write(static_cast<uint8_t>(value));
    return wire_.endTransmission() == 0;
}

bool Tmp112Sensor::readTemperature(int16_t &centiDegrees) {
    if (measure(centiDegrees))
        return true;
    recoverTwiBus(wire_);
    enterShutdown();
    return measure(centiDegrees);
}

bool Tmp112Sensor::measure(int16_t &centiDegrees) {
    uint16_t configurationRegister = 0;
    if (!readRegister16(0x01, configurationRegister))
        return false;

    // In shutdown mode OS=1 starts one conversion. OS reads back as 1 again
    // when conversion is complete; TMP112 specifies at most 35 ms.
    configurationRegister |= 0x8100U;
    if (!writeRegister16(0x01, configurationRegister))
        return false;
    const uint32_t started = millis();
    do {
        delay(1);
        if (!readRegister16(0x01, configurationRegister))
            return false;
    } while ((configurationRegister & 0x8000U) == 0 &&
             static_cast<uint32_t>(millis() - started) < 40U);
    if ((configurationRegister & 0x8000U) == 0)
        return false;

    uint16_t temperatureRegister = 0;
    if (!readRegister16(0x00, temperatureRegister))
        return false;

    const bool extendedMode = (configurationRegister & 0x0010U) != 0;
    const uint8_t shift = extendedMode ? 3 : 4;
    const uint8_t bits = extendedMode ? 13 : 12;
    int16_t raw = static_cast<int16_t>(temperatureRegister >> shift);
    if ((raw & (1 << (bits - 1))) != 0)
        raw |= static_cast<int16_t>(~((1 << bits) - 1));
    const int32_t scaled = static_cast<int32_t>(raw) * 625;
    const int16_t value =
        static_cast<int16_t>(scaled >= 0 ? (scaled + 50) / 100 : (scaled - 50) / 100);
    if (value < -8000 || value > 12500)
        return false;
    centiDegrees = value;
    return true;
}

} // namespace node
} // namespace radiosensors
