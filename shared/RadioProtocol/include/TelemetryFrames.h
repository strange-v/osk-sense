#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JoinRequest.h"

namespace radiosensors {
namespace protocol {

constexpr int16_t kInvalidTemperature = INT16_MIN;
constexpr uint16_t kInvalidHumidity = UINT16_MAX;
constexpr uint16_t kInvalidPressure = UINT16_MAX;
constexpr uint16_t kInvalidSupplyVoltage = UINT16_MAX;
constexpr int8_t kNoDownlinkRssi = INT8_MIN;

constexpr int16_t kMinimumTemperatureCentiDegrees = -8000;
constexpr int16_t kMaximumTemperatureCentiDegrees = 12500;
constexpr uint16_t kMaximumHumidityCentiPercent = 10000;
constexpr uint16_t kMinimumPressureDeciHectopascals = 3000;
constexpr uint16_t kMaximumPressureDeciHectopascals = 11000;

// Radio state byte of the common prefix.
constexpr uint8_t kRadioPowerLevelMask = 0x1F;
constexpr uint8_t kRadioFallback = 0x20;
constexpr uint8_t kRadioStateReservedMask = 0xC0;

// Common prefix: link fields first, then supply voltage next to the
// measurements.
constexpr size_t kTelemetryRadioStateOffset = 1;
constexpr size_t kTelemetryDownlinkRssiOffset = 2;
constexpr size_t kTelemetrySupplyVoltageOffset = 3;
constexpr size_t kTelemetryPrefixSize = 5;
constexpr size_t kVoltageTelemetrySize = 5;
constexpr size_t kTemperatureTelemetrySize = 7;
constexpr size_t kClimateThTelemetrySize = 9;
constexpr size_t kClimateThpTelemetrySize = 11;
constexpr size_t kBinaryTelemetrySize = 6;
constexpr size_t kCounterTelemetrySize = 9;
constexpr size_t kBinaryClimateThTelemetrySize = 10;
constexpr size_t kBinaryTemperatureTelemetrySize = 8;

// Payload of the RFM69 ACK that answers telemetry: empty, a flags byte, or
// the flags byte followed by a power target.
constexpr uint8_t kTelemetryAckCommandPending = 0x01;
constexpr uint8_t kTelemetryAckPowerTarget = 0x02;
constexpr size_t kMaxTelemetryAckSize = 2;

enum class TelemetryCodecStatus : uint8_t {
    Ok,
    OutputTooSmall,
    WrongLength,
    WrongHeader,
    InvalidRadioState,
    InvalidState,
    InvalidTemperature,
    InvalidHumidity,
    InvalidPressure,
};

// The node's own view of its supply and radio, carried by every report.
struct TelemetryPrefix {
    uint16_t supplyMillivolts;
    uint8_t radioState;
    int8_t downlinkRssi;
};

struct TelemetryView {
    uint16_t supplyMillivolts;
    uint8_t radioState;
    int8_t downlinkRssi;
    const uint8_t *profilePayload;
    size_t profilePayloadSize;
};

struct TelemetryAck {
    bool commandPending;
    bool hasPowerTarget;
    uint8_t powerTarget;
};

constexpr uint8_t encodeRadioState(const uint8_t powerLevel, const bool fallback) {
    return static_cast<uint8_t>((powerLevel & kRadioPowerLevelMask) |
                                (fallback ? kRadioFallback : 0));
}

constexpr uint8_t radioPowerLevel(const uint8_t radioState) {
    return radioState & kRadioPowerLevelMask;
}

// The strength of a received acknowledgement, kept clear of the no-value
// sentinel. RFM69 reports -127..0 dBm.
inline int8_t downlinkRssiValue(const int16_t rssi) {
    if (rssi < -127)
        return -127;
    if (rssi > 0)
        return 0;
    return static_cast<int8_t>(rssi);
}

inline bool validTemperature(const int16_t value) {
    return value == kInvalidTemperature ||
           (value >= kMinimumTemperatureCentiDegrees && value <= kMaximumTemperatureCentiDegrees);
}

inline bool validHumidity(const uint16_t value) {
    return value == kInvalidHumidity || value <= kMaximumHumidityCentiPercent;
}

inline bool validPressure(const uint16_t value) {
    return value == kInvalidPressure ||
           (value >= kMinimumPressureDeciHectopascals && value <= kMaximumPressureDeciHectopascals);
}

inline TelemetryCodecStatus beginTelemetry(const TelemetryPrefix &prefix,
                                           uint8_t *const output,
                                           const size_t capacity,
                                           const size_t required) {
    if ((prefix.radioState & kRadioStateReservedMask) != 0)
        return TelemetryCodecStatus::InvalidRadioState;
    if (output == nullptr || capacity < required)
        return TelemetryCodecStatus::OutputTooSmall;
    output[0] = encodeHeader(FrameKind::Telemetry);
    output[kTelemetryRadioStateOffset] = prefix.radioState;
    output[kTelemetryDownlinkRssiOffset] = static_cast<uint8_t>(prefix.downlinkRssi);
    writeUint16Le(output + kTelemetrySupplyVoltageOffset, prefix.supplyMillivolts);
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus
decodeTelemetry(const uint8_t *const input, const size_t size, TelemetryView &view) {
    if (input == nullptr || size < kTelemetryPrefixSize)
        return TelemetryCodecStatus::WrongLength;
    if (input[0] != encodeHeader(FrameKind::Telemetry))
        return TelemetryCodecStatus::WrongHeader;
    if ((input[kTelemetryRadioStateOffset] & kRadioStateReservedMask) != 0)
        return TelemetryCodecStatus::InvalidRadioState;
    view.radioState = input[kTelemetryRadioStateOffset];
    view.downlinkRssi = static_cast<int8_t>(input[kTelemetryDownlinkRssiOffset]);
    view.supplyMillivolts = readUint16Le(input + kTelemetrySupplyVoltageOffset);
    view.profilePayload = input + kTelemetryPrefixSize;
    view.profilePayloadSize = size - kTelemetryPrefixSize;
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeVoltageTelemetry(const TelemetryPrefix &prefix,
                                                   uint8_t *const output,
                                                   const size_t capacity) {
    return beginTelemetry(prefix, output, capacity, kVoltageTelemetrySize);
}

inline TelemetryCodecStatus encodeTemperatureTelemetry(const TelemetryPrefix &prefix,
                                                       const int16_t temperatureCentiDegrees,
                                                       uint8_t *const output,
                                                       const size_t capacity) {
    if (!validTemperature(temperatureCentiDegrees))
        return TelemetryCodecStatus::InvalidTemperature;
    const auto status = beginTelemetry(prefix, output, capacity, kTemperatureTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    writeUint16Le(output + 5, static_cast<uint16_t>(temperatureCentiDegrees));
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeClimateThTelemetry(const TelemetryPrefix &prefix,
                                                     const int16_t temperatureCentiDegrees,
                                                     const uint16_t humidityCentiPercent,
                                                     uint8_t *const output,
                                                     const size_t capacity) {
    if (!validTemperature(temperatureCentiDegrees))
        return TelemetryCodecStatus::InvalidTemperature;
    if (!validHumidity(humidityCentiPercent))
        return TelemetryCodecStatus::InvalidHumidity;
    const auto status = beginTelemetry(prefix, output, capacity, kClimateThTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    writeUint16Le(output + 5, static_cast<uint16_t>(temperatureCentiDegrees));
    writeUint16Le(output + 7, humidityCentiPercent);
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeClimateThpTelemetry(const TelemetryPrefix &prefix,
                                                      const int16_t temperatureCentiDegrees,
                                                      const uint16_t humidityCentiPercent,
                                                      const uint16_t pressureDeciHectopascals,
                                                      uint8_t *const output,
                                                      const size_t capacity) {
    if (!validTemperature(temperatureCentiDegrees))
        return TelemetryCodecStatus::InvalidTemperature;
    if (!validHumidity(humidityCentiPercent))
        return TelemetryCodecStatus::InvalidHumidity;
    if (!validPressure(pressureDeciHectopascals))
        return TelemetryCodecStatus::InvalidPressure;
    const auto status = beginTelemetry(prefix, output, capacity, kClimateThpTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    writeUint16Le(output + 5, static_cast<uint16_t>(temperatureCentiDegrees));
    writeUint16Le(output + 7, humidityCentiPercent);
    writeUint16Le(output + 9, pressureDeciHectopascals);
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeBinaryTelemetry(const TelemetryPrefix &prefix,
                                                  const uint8_t state,
                                                  uint8_t *const output,
                                                  const size_t capacity) {
    if (state > 1)
        return TelemetryCodecStatus::InvalidState;
    const auto status = beginTelemetry(prefix, output, capacity, kBinaryTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    output[5] = state;
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeCounterTelemetry(const TelemetryPrefix &prefix,
                                                   const uint32_t count,
                                                   uint8_t *const output,
                                                   const size_t capacity) {
    const auto status = beginTelemetry(prefix, output, capacity, kCounterTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    writeUint32Le(output + 5, count);
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeBinaryClimateThTelemetry(const TelemetryPrefix &prefix,
                                                           const uint8_t state,
                                                           const int16_t temperatureCentiDegrees,
                                                           const uint16_t humidityCentiPercent,
                                                           uint8_t *const output,
                                                           const size_t capacity) {
    if (state > 1)
        return TelemetryCodecStatus::InvalidState;
    if (!validTemperature(temperatureCentiDegrees))
        return TelemetryCodecStatus::InvalidTemperature;
    if (!validHumidity(humidityCentiPercent))
        return TelemetryCodecStatus::InvalidHumidity;
    const auto status = beginTelemetry(prefix, output, capacity, kBinaryClimateThTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    output[5] = state;
    writeUint16Le(output + 6, static_cast<uint16_t>(temperatureCentiDegrees));
    writeUint16Le(output + 8, humidityCentiPercent);
    return TelemetryCodecStatus::Ok;
}

inline TelemetryCodecStatus encodeBinaryTemperatureTelemetry(const TelemetryPrefix &prefix,
                                                             const uint8_t state,
                                                             const int16_t temperatureCentiDegrees,
                                                             uint8_t *const output,
                                                             const size_t capacity) {
    if (state > 1)
        return TelemetryCodecStatus::InvalidState;
    if (!validTemperature(temperatureCentiDegrees))
        return TelemetryCodecStatus::InvalidTemperature;
    const auto status = beginTelemetry(prefix, output, capacity, kBinaryTemperatureTelemetrySize);
    if (status != TelemetryCodecStatus::Ok)
        return status;
    output[5] = state;
    writeUint16Le(output + 6, static_cast<uint16_t>(temperatureCentiDegrees));
    return TelemetryCodecStatus::Ok;
}

// An empty acknowledgement says nothing beyond the acknowledgement itself and
// encodes to zero bytes. An out-of-range target is dropped, never truncated.
inline size_t
encodeTelemetryAck(const TelemetryAck &ack, uint8_t *const output, const size_t capacity) {
    const bool target = ack.hasPowerTarget && ack.powerTarget <= kMaxRadioPowerLevel;
    const uint8_t flags =
        static_cast<uint8_t>((ack.commandPending ? kTelemetryAckCommandPending : 0) |
                             (target ? kTelemetryAckPowerTarget : 0));
    const size_t size = flags == 0 ? 0 : (target ? 2 : 1);
    if (size == 0 || output == nullptr || capacity < size)
        return 0;
    output[0] = flags;
    if (target)
        output[1] = ack.powerTarget;
    return size;
}

// Unknown flags are ignored, and so is a missing or out-of-range target.
inline TelemetryAck decodeTelemetryAck(const uint8_t *const data, const size_t size) {
    TelemetryAck ack{false, false, 0};
    if (data == nullptr || size == 0)
        return ack;
    ack.commandPending = (data[0] & kTelemetryAckCommandPending) != 0;
    if ((data[0] & kTelemetryAckPowerTarget) != 0 && size >= 2 && data[1] <= kMaxRadioPowerLevel) {
        ack.hasPowerTarget = true;
        ack.powerTarget = data[1];
    }
    return ack;
}

} // namespace protocol
} // namespace radiosensors
