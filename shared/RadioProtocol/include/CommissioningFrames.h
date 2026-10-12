#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JoinRequest.h"

namespace radiosensors {
namespace protocol {

constexpr size_t kInstallationKeySize = 16;
constexpr size_t kJoinAcceptSize = 34;
constexpr size_t kJoinConfirmSize = 15;
constexpr size_t kJoinCompleteSize = 15;

struct JoinAccept {
    uint8_t deviceUid[kDeviceUidSize];
    uint32_t requestNonce;
    uint8_t assignedNodeId;
    uint8_t gatewayNodeId;
    uint8_t networkId;
    uint8_t installationKey[kInstallationKeySize];
};

struct JoinConfirm {
    uint8_t deviceUid[kDeviceUidSize];
    uint32_t requestNonce;
};

using JoinComplete = JoinConfirm;

enum class CommissioningCodecStatus : uint8_t {
    Ok,
    OutputTooSmall,
    EmptyFrame,
    UnsupportedVersion,
    UnsupportedKind,
    WrongFrameKind,
    WrongLength,
    InvalidNodeId,
};

inline CommissioningCodecStatus mapFrameStatus(const DecodeStatus status) {
    switch (status) {
    case DecodeStatus::EmptyFrame:
        return CommissioningCodecStatus::EmptyFrame;
    case DecodeStatus::UnsupportedVersion:
        return CommissioningCodecStatus::UnsupportedVersion;
    case DecodeStatus::UnsupportedKind:
        return CommissioningCodecStatus::UnsupportedKind;
    case DecodeStatus::Ok:
        return CommissioningCodecStatus::Ok;
    }
    return CommissioningCodecStatus::UnsupportedKind;
}

inline CommissioningCodecStatus
encodeJoinAccept(const JoinAccept &value, uint8_t *const output, const size_t capacity) {
    if (output == nullptr || capacity < kJoinAcceptSize)
        return CommissioningCodecStatus::OutputTooSmall;
    if (value.assignedNodeId == 0 || value.assignedNodeId == value.gatewayNodeId ||
        value.assignedNodeId == 255 || value.gatewayNodeId == 0 || value.gatewayNodeId == 255) {
        return CommissioningCodecStatus::InvalidNodeId;
    }
    output[0] = encodeHeader(FrameKind::JoinAccept);
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        output[1 + i] = value.deviceUid[i];
    writeUint32Le(output + 11, value.requestNonce);
    output[15] = value.assignedNodeId;
    output[16] = value.gatewayNodeId;
    output[17] = value.networkId;
    for (size_t i = 0; i < kInstallationKeySize; ++i)
        output[18 + i] = value.installationKey[i];
    return CommissioningCodecStatus::Ok;
}

inline CommissioningCodecStatus
decodeJoinAccept(const uint8_t *data, const size_t size, JoinAccept &value) {
    FrameView frame{};
    const DecodeStatus frameStatus = decodeFrame(data, size, frame);
    if (frameStatus != DecodeStatus::Ok)
        return mapFrameStatus(frameStatus);
    if (frame.kind != FrameKind::JoinAccept)
        return CommissioningCodecStatus::WrongFrameKind;
    if (size != kJoinAcceptSize)
        return CommissioningCodecStatus::WrongLength;
    if (data[15] == 0 || data[15] == data[16] || data[15] == 255 || data[16] == 0 ||
        data[16] == 255) {
        return CommissioningCodecStatus::InvalidNodeId;
    }
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        value.deviceUid[i] = data[1 + i];
    value.requestNonce = readUint32Le(data + 11);
    value.assignedNodeId = data[15];
    value.gatewayNodeId = data[16];
    value.networkId = data[17];
    for (size_t i = 0; i < kInstallationKeySize; ++i)
        value.installationKey[i] = data[18 + i];
    return CommissioningCodecStatus::Ok;
}

inline CommissioningCodecStatus
encodeJoinConfirm(const JoinConfirm &value, uint8_t *const output, const size_t capacity) {
    if (output == nullptr || capacity < kJoinConfirmSize)
        return CommissioningCodecStatus::OutputTooSmall;
    output[0] = encodeHeader(FrameKind::JoinConfirm);
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        output[1 + i] = value.deviceUid[i];
    writeUint32Le(output + 11, value.requestNonce);
    return CommissioningCodecStatus::Ok;
}

inline CommissioningCodecStatus
decodeJoinConfirm(const uint8_t *data, const size_t size, JoinConfirm &value) {
    FrameView frame{};
    const DecodeStatus frameStatus = decodeFrame(data, size, frame);
    if (frameStatus != DecodeStatus::Ok)
        return mapFrameStatus(frameStatus);
    if (frame.kind != FrameKind::JoinConfirm)
        return CommissioningCodecStatus::WrongFrameKind;
    if (size != kJoinConfirmSize)
        return CommissioningCodecStatus::WrongLength;
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        value.deviceUid[i] = data[1 + i];
    value.requestNonce = readUint32Le(data + 11);
    return CommissioningCodecStatus::Ok;
}

inline CommissioningCodecStatus
encodeJoinComplete(const JoinComplete &value, uint8_t *const output, const size_t capacity) {
    if (output == nullptr || capacity < kJoinCompleteSize)
        return CommissioningCodecStatus::OutputTooSmall;
    output[0] = encodeHeader(FrameKind::JoinComplete);
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        output[1 + i] = value.deviceUid[i];
    writeUint32Le(output + 11, value.requestNonce);
    return CommissioningCodecStatus::Ok;
}

inline CommissioningCodecStatus
decodeJoinComplete(const uint8_t *data, const size_t size, JoinComplete &value) {
    FrameView frame{};
    const DecodeStatus frameStatus = decodeFrame(data, size, frame);
    if (frameStatus != DecodeStatus::Ok)
        return mapFrameStatus(frameStatus);
    if (frame.kind != FrameKind::JoinComplete)
        return CommissioningCodecStatus::WrongFrameKind;
    if (size != kJoinCompleteSize)
        return CommissioningCodecStatus::WrongLength;
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        value.deviceUid[i] = data[1 + i];
    value.requestNonce = readUint32Le(data + 11);
    return CommissioningCodecStatus::Ok;
}

} // namespace protocol
} // namespace radiosensors
