#pragma once

#include <stddef.h>
#include <stdint.h>

#include "RadioProtocol.h"

namespace radiosensors {
namespace protocol {

constexpr size_t kDeviceUidSize = 10;
constexpr size_t kJoinRequestSize = 21;
constexpr uint16_t kUnassignedProfileId = 0;

struct FirmwareVersion {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

struct JoinRequest {
    uint8_t deviceUid[kDeviceUidSize];
    uint16_t profileId;
    FirmwareVersion firmware;
    uint32_t requestNonce;
    // Transmit power ceiling of this node's hardware and supply.
    uint8_t maxPowerLevel;
};

enum class JoinRequestStatus : uint8_t {
    Ok,
    OutputTooSmall,
    EmptyFrame,
    UnsupportedVersion,
    UnsupportedKind,
    WrongFrameKind,
    WrongLength,
    InvalidProfileId,
    InvalidPowerLevel,
};

inline void writeUint16Le(uint8_t *const output, const uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeUint32Le(uint8_t *const output, const uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
    output[2] = static_cast<uint8_t>(value >> 16);
    output[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t readUint16Le(const uint8_t *const input) {
    return static_cast<uint16_t>(input[0]) | (static_cast<uint16_t>(input[1]) << 8);
}

inline uint32_t readUint32Le(const uint8_t *const input) {
    return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8) |
           (static_cast<uint32_t>(input[2]) << 16) | (static_cast<uint32_t>(input[3]) << 24);
}

inline JoinRequestStatus
encodeJoinRequest(const JoinRequest &request, uint8_t *const output, const size_t outputSize) {
    if (output == nullptr || outputSize < kJoinRequestSize)
        return JoinRequestStatus::OutputTooSmall;
    if (request.profileId == kUnassignedProfileId)
        return JoinRequestStatus::InvalidProfileId;
    if (request.maxPowerLevel > kMaxRadioPowerLevel)
        return JoinRequestStatus::InvalidPowerLevel;

    output[0] = encodeHeader(FrameKind::JoinRequest);
    for (size_t index = 0; index < kDeviceUidSize; ++index)
        output[1 + index] = request.deviceUid[index];
    writeUint16Le(output + 11, request.profileId);
    output[13] = request.firmware.major;
    output[14] = request.firmware.minor;
    output[15] = request.firmware.patch;
    writeUint32Le(output + 16, request.requestNonce);
    output[20] = request.maxPowerLevel;
    return JoinRequestStatus::Ok;
}

inline JoinRequestStatus
decodeJoinRequest(const uint8_t *const data, const size_t size, JoinRequest &request) {
    FrameView frame{};
    switch (decodeFrame(data, size, frame)) {
    case DecodeStatus::EmptyFrame:
        return JoinRequestStatus::EmptyFrame;
    case DecodeStatus::UnsupportedVersion:
        return JoinRequestStatus::UnsupportedVersion;
    case DecodeStatus::UnsupportedKind:
        return JoinRequestStatus::UnsupportedKind;
    case DecodeStatus::Ok:
        break;
    }

    if (frame.kind != FrameKind::JoinRequest)
        return JoinRequestStatus::WrongFrameKind;
    if (size != kJoinRequestSize)
        return JoinRequestStatus::WrongLength;

    const uint16_t profileId = readUint16Le(data + 11);
    if (profileId == kUnassignedProfileId)
        return JoinRequestStatus::InvalidProfileId;
    if (data[20] > kMaxRadioPowerLevel)
        return JoinRequestStatus::InvalidPowerLevel;

    for (size_t index = 0; index < kDeviceUidSize; ++index)
        request.deviceUid[index] = data[1 + index];
    request.profileId = profileId;
    request.firmware = FirmwareVersion{data[13], data[14], data[15]};
    request.requestNonce = readUint32Le(data + 16);
    request.maxPowerLevel = data[20];
    return JoinRequestStatus::Ok;
}

} // namespace protocol
} // namespace radiosensors
