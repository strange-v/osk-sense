#pragma once

#include <stddef.h>
#include <stdint.h>

namespace radiosensors {
namespace protocol {

constexpr uint8_t kProtocolMajor = 2;
constexpr uint8_t kVersionShift = 5;
constexpr uint8_t kVersionMask = 0xE0;
constexpr uint8_t kKindMask = 0x1F;
constexpr size_t kHeaderSize = 1;
constexpr uint8_t kMaxRadioPowerLevel = 31;

enum class FrameKind : uint8_t {
    Telemetry = 0,
    JoinRequest = 1,
    JoinAccept = 2,
    JoinConfirm = 3,
    Command = 4,
    CommandResult = 5,
    Error = 6,
    JoinComplete = 7,
    CommandReady = 8,
    NoCommand = 9,
};

enum class DecodeStatus : uint8_t {
    Ok,
    EmptyFrame,
    UnsupportedVersion,
    UnsupportedKind,
};

struct FrameView {
    FrameKind kind;
    const uint8_t *payload;
    size_t payloadSize;
};

constexpr bool isKnownFrameKind(const uint8_t rawKind) {
    return rawKind <= static_cast<uint8_t>(FrameKind::NoCommand);
}

constexpr uint8_t encodeHeader(const FrameKind kind) {
    return static_cast<uint8_t>((kProtocolMajor << kVersionShift) |
                                (static_cast<uint8_t>(kind) & kKindMask));
}

static_assert(encodeHeader(FrameKind::Telemetry) == 0x40, "v2 telemetry header changed");
static_assert(encodeHeader(FrameKind::JoinRequest) == 0x41, "v2 join-request header changed");
static_assert(encodeHeader(FrameKind::Error) == 0x46, "v2 error header changed");
static_assert(encodeHeader(FrameKind::CommandReady) == 0x48, "v2 command-ready header changed");
static_assert(encodeHeader(FrameKind::NoCommand) == 0x49, "v2 no-command header changed");

inline DecodeStatus decodeFrame(const uint8_t *const data, const size_t size, FrameView &frame) {
    if (data == nullptr || size < kHeaderSize)
        return DecodeStatus::EmptyFrame;

    const uint8_t header = data[0];
    const uint8_t version = (header & kVersionMask) >> kVersionShift;
    if (version != kProtocolMajor)
        return DecodeStatus::UnsupportedVersion;

    const uint8_t rawKind = header & kKindMask;
    if (!isKnownFrameKind(rawKind))
        return DecodeStatus::UnsupportedKind;

    frame.kind = static_cast<FrameKind>(rawKind);
    frame.payload = data + kHeaderSize;
    frame.payloadSize = size - kHeaderSize;
    return DecodeStatus::Ok;
}

} // namespace protocol
} // namespace radiosensors
