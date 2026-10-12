#pragma once

#include <stddef.h>
#include <stdint.h>

namespace radiosensors::stream {

// A consumer must ignore an unknown message kind, or every later addition
// breaks every deployed client.
constexpr uint8_t kVersion = 1;
constexpr size_t kCommonPrefixSize = 6;
constexpr size_t kIdentitySize = 16;
constexpr size_t kSnapshotControlFrameSize = 10;
constexpr size_t kRegistryChangedFrameSize = 10;
constexpr size_t kTelemetryEnvelopeSize = 20;
constexpr size_t kHelloFrameSize = 42;

enum class MessageKind : uint8_t {
    // Always the first server-to-client message.
    Hello = 1,
    SnapshotBegin = 2,
    Telemetry = 3,
    SnapshotEnd = 4,
    // Carries the new durable generation; the consumer refetches
    // /api/nodes. See WEBSOCKET.md for why polling cannot replace it.
    RegistryChanged = 5,
};

inline void writeUint16(uint8_t *const output, const uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
}

inline void writeUint32(uint8_t *const output, const uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
    output[2] = static_cast<uint8_t>(value >> 16U);
    output[3] = static_cast<uint8_t>(value >> 24U);
}

inline void writeUint64(uint8_t *const output, const uint64_t value) {
    writeUint32(output, static_cast<uint32_t>(value));
    writeUint32(output + 4, static_cast<uint32_t>(value >> 32U));
}

inline size_t encodeSnapshotControl(const MessageKind kind,
                                    const uint32_t sequence,
                                    const uint32_t registryGeneration,
                                    uint8_t *const output,
                                    const size_t capacity) {
    if (output == nullptr || capacity < kSnapshotControlFrameSize ||
        (kind != MessageKind::SnapshotBegin && kind != MessageKind::SnapshotEnd)) {
        return 0;
    }
    output[0] = kVersion;
    output[1] = static_cast<uint8_t>(kind);
    writeUint32(output + 2, sequence);
    writeUint32(output + 6, registryGeneration);
    return kSnapshotControlFrameSize;
}

inline size_t encodeHello(const uint32_t sequence,
                          const uint8_t *const gatewayId,
                          const uint8_t *const bootId,
                          const uint32_t registryGeneration,
                          uint8_t *const output,
                          const size_t capacity) {
    if (output == nullptr || capacity < kHelloFrameSize || gatewayId == nullptr ||
        bootId == nullptr) {
        return 0;
    }
    output[0] = kVersion;
    output[1] = static_cast<uint8_t>(MessageKind::Hello);
    writeUint32(output + 2, sequence);
    for (size_t index = 0; index < kIdentitySize; ++index) {
        output[6 + index] = gatewayId[index];
        output[22 + index] = bootId[index];
    }
    writeUint32(output + 38, registryGeneration);
    return kHelloFrameSize;
}

// The sequence is the telemetry watermark, as in the snapshot control frames,
// so a consumer can order this against telemetry it has already applied.
inline size_t encodeRegistryChanged(const uint32_t sequence,
                                    const uint32_t registryGeneration,
                                    uint8_t *const output,
                                    const size_t capacity) {
    if (output == nullptr || capacity < kRegistryChangedFrameSize)
        return 0;
    output[0] = kVersion;
    output[1] = static_cast<uint8_t>(MessageKind::RegistryChanged);
    writeUint32(output + 2, sequence);
    writeUint32(output + 6, registryGeneration);
    return kRegistryChangedFrameSize;
}

inline size_t encodeTelemetry(const uint32_t sequence,
                              const uint8_t nodeId,
                              const uint16_t profileId,
                              const uint64_t receivedAtUnixMs,
                              const int16_t rssi,
                              const uint8_t *const payload,
                              const size_t payloadSize,
                              uint8_t *const output,
                              const size_t capacity) {
    const size_t requiredSize = kTelemetryEnvelopeSize + payloadSize;
    if (output == nullptr || capacity < requiredSize || payloadSize > UINT8_MAX ||
        (payloadSize != 0 && payload == nullptr)) {
        return 0;
    }
    output[0] = kVersion;
    output[1] = static_cast<uint8_t>(MessageKind::Telemetry);
    writeUint32(output + 2, sequence);
    output[6] = nodeId;
    writeUint16(output + 7, profileId);
    writeUint64(output + 9, receivedAtUnixMs);
    writeUint16(output + 17, static_cast<uint16_t>(rssi));
    output[19] = static_cast<uint8_t>(payloadSize);
    for (size_t index = 0; index < payloadSize; ++index)
        output[kTelemetryEnvelopeSize + index] = payload[index];
    return requiredSize;
}

} // namespace radiosensors::stream
