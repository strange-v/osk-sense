#include "Sht40Sensor.h"

#include "TwiBusRecovery.h"

namespace radiosensors {
namespace node {

void Sht40Sensor::begin() {
    wire_.begin();
}

uint8_t Sht40Sensor::crc8(const uint8_t *data, const uint8_t size) {
    uint8_t crc = 0xFF;
    for (uint8_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0 ? static_cast<uint8_t>((crc << 1) ^ 0x31U)
                                     : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

bool Sht40Sensor::read(int16_t &temperatureCentiDegrees, uint16_t &humidityCentiPercent) {
    if (measure(temperatureCentiDegrees, humidityCentiPercent))
        return true;
    recoverTwiBus(wire_);
    softReset();
    return measure(temperatureCentiDegrees, humidityCentiPercent);
}

void Sht40Sensor::softReset() {
    wire_.beginTransmission(address_);
    wire_.write(0x94);
    wire_.endTransmission();
    // The reset takes at most 1 ms.
    delay(1);
}

bool Sht40Sensor::measure(int16_t &temperatureCentiDegrees, uint16_t &humidityCentiPercent) {
    // 0xFD: high-precision measurement without heater.
    wire_.beginTransmission(address_);
    wire_.write(0xFD);
    if (wire_.endTransmission() != 0)
        return false;
    delay(10);

    uint8_t bytes[6];
    if (wire_.requestFrom(address_, static_cast<uint8_t>(6)) != 6)
        return false;
    for (uint8_t index = 0; index < sizeof(bytes); ++index)
        bytes[index] = static_cast<uint8_t>(wire_.read());
    if (crc8(bytes, 2) != bytes[2] || crc8(bytes + 3, 2) != bytes[5])
        return false;

    const uint16_t rawTemperature = static_cast<uint16_t>(bytes[0]) << 8 | bytes[1];
    const uint16_t rawHumidity = static_cast<uint16_t>(bytes[3]) << 8 | bytes[4];
    const int32_t temperature = -4500L + (17500L * rawTemperature + 32767L) / 65535L;
    int32_t humidity = -600L + (12500L * rawHumidity + 32767L) / 65535L;
    if (humidity < 0)
        humidity = 0;
    if (humidity > 10000)
        humidity = 10000;

    if (temperature < -4000 || temperature > 12500)
        return false;
    temperatureCentiDegrees = static_cast<int16_t>(temperature);
    humidityCentiPercent = static_cast<uint16_t>(humidity);
    return true;
}

} // namespace node
} // namespace radiosensors
