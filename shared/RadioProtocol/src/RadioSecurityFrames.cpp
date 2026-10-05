#include "RadioSecurityFrames.h"

#include <string.h>

namespace radiosensors {
namespace security {
namespace frames {
namespace {
void encodeIdentity(uint8_t* raw, uint8_t header, const JoinIdentity& value) {
    raw[0] = header;
    memcpy(raw + 1, value.uid, protocol::kDeviceUidSize);
    for (uint8_t i = 0; i < 8; ++i)
        raw[11 + i] = static_cast<uint8_t>(value.requestNonce >> (8 * i));
}
void decodeIdentity(const uint8_t* raw, JoinIdentity& value) {
    memcpy(value.uid, raw + 1, protocol::kDeviceUidSize);
    value.requestNonce = 0;
    for (uint8_t i = 0; i < 8; ++i)
        value.requestNonce |= static_cast<uint64_t>(raw[11 + i]) << (8 * i);
}
bool authentic(const osk::crypto::Cmac& mac, Transport transport,
               const uint8_t* wire, size_t size, uint8_t header,
               size_t expected, size_t tagSize, const uint8_t* salt = nullptr) {
    return wire && size == expected && wire[0] == header &&
        verifyJoin(mac, transport, wire, size, tagSize, salt);
}
bool seal(const osk::crypto::Cmac& mac, Transport transport,
          const uint8_t* raw, size_t size, size_t tagSize,
          uint8_t* output, size_t capacity, const uint8_t* salt = nullptr) {
    size_t encoded = 0;
    return sealJoin(mac, transport, raw, size, tagSize, output, capacity, encoded, salt);
}
bool validAccept(const JoinAccept& value) {
    return value.nodeId >= 1 && value.nodeId <= 99 && value.networkId != 0;
}
}

bool sealJoinRequest(const osk::crypto::Cmac& factoryMac, Transport transport,
                     const JoinRequest& value, uint8_t* output, size_t capacity) {
    if (!value.profileId || value.maxPowerLevel > protocol::kMaxRadioPowerLevel) return false;
    uint8_t raw[25];
    encodeIdentity(raw, kJoinRequestHeader, value.identity);
    protocol::writeUint16Le(raw + 19, value.profileId);
    raw[21] = value.firmware.major;
    raw[22] = value.firmware.minor;
    raw[23] = value.firmware.patch;
    raw[24] = value.maxPowerLevel;
    return seal(factoryMac, transport, raw, sizeof(raw), kNodeTagSize, output, capacity);
}

bool openJoinRequest(const osk::crypto::Cmac& factoryMac, Transport transport,
                     const uint8_t* wire, size_t size, JoinRequest& value) {
    if (!authentic(factoryMac, transport, wire, size, kJoinRequestHeader, kJoinRequestSize,
                   kNodeTagSize)) return false;
    JoinRequest candidate;
    decodeIdentity(wire, candidate.identity);
    candidate.profileId = protocol::readUint16Le(wire + 19);
    candidate.firmware = protocol::FirmwareVersion{wire[21], wire[22], wire[23]};
    candidate.maxPowerLevel = wire[24];
    if (!candidate.profileId || candidate.maxPowerLevel > protocol::kMaxRadioPowerLevel) return false;
    value = candidate;
    return true;
}

bool sealJoinAccept(const osk::crypto::Cmac& factoryMac, Transport transport,
                    const JoinAccept& value, uint8_t* output, size_t capacity) {
    if (!validAccept(value)) return false;
    uint8_t raw[29];
    encodeIdentity(raw, kJoinAcceptHeader, value.identity);
    memcpy(raw + 19, value.salt, kSaltSize);
    raw[27] = value.nodeId;
    raw[28] = value.networkId;
    return seal(factoryMac, transport, raw, sizeof(raw), kGatewayTagSize, output, capacity);
}

bool openJoinAccept(const osk::crypto::Cmac& factoryMac, Transport transport,
                    const uint8_t* wire, size_t size, JoinAccept& value) {
    if (!authentic(factoryMac, transport, wire, size, kJoinAcceptHeader, kJoinAcceptSize,
                   kGatewayTagSize)) return false;
    JoinAccept candidate;
    decodeIdentity(wire, candidate.identity);
    memcpy(candidate.salt, wire + 19, kSaltSize);
    candidate.nodeId = wire[27];
    candidate.networkId = wire[28];
    if (!validAccept(candidate)) return false;
    value = candidate;
    return true;
}

bool sealJoinProof(const osk::crypto::Cmac& sessionMac, Transport transport,
                   const JoinIdentity& value, const uint8_t (&salt)[kSaltSize],
                   bool complete, uint8_t* output, size_t capacity) {
    uint8_t raw[19];
    encodeIdentity(raw, complete ? kJoinCompleteHeader : kJoinConfirmHeader, value);
    return seal(sessionMac, transport, raw, sizeof(raw), complete ? kGatewayTagSize : kNodeTagSize,
                output, capacity, salt);
}

bool openJoinProof(const osk::crypto::Cmac& sessionMac, Transport transport,
                   const uint8_t* wire, size_t size, const uint8_t (&salt)[kSaltSize],
                   bool complete, JoinIdentity& value) {
    if (!authentic(sessionMac, transport, wire, size,
                   complete ? kJoinCompleteHeader : kJoinConfirmHeader,
                   complete ? kJoinCompleteSize : kJoinConfirmSize,
                   complete ? kGatewayTagSize : kNodeTagSize, salt)) return false;
    decodeIdentity(wire, value);
    return true;
}

bool encodeCommand(const Command& value, uint8_t* output, size_t capacity, size_t& size) {
    size = 0;
    if (!value.commandId || value.argumentSize > sizeof(value.arguments) || !output ||
        capacity < kCommandEnvelopeSize + value.argumentSize) return false;
    protocol::writeUint16Le(output, value.commandId);
    output[2] = value.type;
    memcpy(output + kCommandEnvelopeSize, value.arguments, value.argumentSize);
    size = kCommandEnvelopeSize + value.argumentSize;
    return true;
}

bool decodeCommand(const uint8_t* payload, size_t size, Command& value) {
    if (!payload || size < kCommandEnvelopeSize || size > kMaxCommandPayloadSize ||
        !protocol::readUint16Le(payload)) return false;
    Command candidate;
    candidate.commandId = protocol::readUint16Le(payload);
    candidate.type = payload[2];
    candidate.argumentSize = static_cast<uint8_t>(size - kCommandEnvelopeSize);
    memcpy(candidate.arguments, payload + kCommandEnvelopeSize, candidate.argumentSize);
    value = candidate;
    return true;
}

bool encodeCommandResult(const CommandResult& value, uint8_t* output, size_t capacity, size_t& size) {
    size = 0;
    if (!value.commandId || !protocol::validCommandStatus(static_cast<uint8_t>(value.status)) ||
        value.dataSize > sizeof(value.data) || !output ||
        capacity < kCommandEnvelopeSize + value.dataSize) return false;
    protocol::writeUint16Le(output, value.commandId);
    output[2] = static_cast<uint8_t>(value.status);
    memcpy(output + kCommandEnvelopeSize, value.data, value.dataSize);
    size = kCommandEnvelopeSize + value.dataSize;
    return true;
}

bool decodeCommandResult(const uint8_t* payload, size_t size, CommandResult& value) {
    if (!payload || size < kCommandEnvelopeSize || size > kMaxCommandPayloadSize ||
        !protocol::readUint16Le(payload) || !protocol::validCommandStatus(payload[2])) return false;
    CommandResult candidate;
    candidate.commandId = protocol::readUint16Le(payload);
    candidate.status = static_cast<protocol::CommandStatus>(payload[2]);
    candidate.dataSize = static_cast<uint8_t>(size - kCommandEnvelopeSize);
    memcpy(candidate.data, payload + kCommandEnvelopeSize, candidate.dataSize);
    value = candidate;
    return true;
}

bool openCommandReply(const Context& context, uint8_t nodeId, Transport transport,
                      uint32_t readyCounter, uint8_t* wire, size_t size,
                      Command& command, bool& hasCommand) {
    if (!wire || size < 5 + kGatewayTagSize ||
        (wire[0] != kCommandHeader && wire[0] != kNoCommandHeader) ||
        protocol::readUint32Le(wire + 1) != readyCounter) return false;
    uint32_t counter;
    uint8_t* payload;
    size_t payloadSize;
    if (!security::open(context, Direction::Gateway, nodeId, transport, wire, size,
                        counter, payload, payloadSize)) return false;
    if (wire[0] == kNoCommandHeader) {
        if (payloadSize != 0) return false;
        hasCommand = false;
        return true;
    }
    if (!decodeCommand(payload, payloadSize, command)) return false;
    hasCommand = true;
    return true;
}

}  // namespace frames
}  // namespace security
}  // namespace radiosensors
