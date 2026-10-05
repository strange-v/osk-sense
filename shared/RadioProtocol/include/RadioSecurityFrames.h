#pragma once

#include "RadioSecurity.h"
#include "CommandSessionFrames.h"

namespace radiosensors {
namespace security {
namespace frames {

constexpr uint8_t kJoinRequestHeader = 0x61;
constexpr uint8_t kJoinAcceptHeader = 0x62;
constexpr uint8_t kJoinConfirmHeader = 0x63;
constexpr uint8_t kJoinCompleteHeader = 0x67;
constexpr uint8_t kCommandHeader = 0x64;
constexpr uint8_t kCommandResultHeader = 0x65;
constexpr uint8_t kCommandReadyHeader = 0x68;
constexpr uint8_t kNoCommandHeader = 0x69;
constexpr size_t kJoinRequestSize = 25 + kNodeTagSize;
constexpr size_t kJoinAcceptSize = 29 + kGatewayTagSize;
constexpr size_t kJoinConfirmSize = 19 + kNodeTagSize;
constexpr size_t kJoinCompleteSize = 19 + kGatewayTagSize;
constexpr size_t kCommandEnvelopeSize = 3;
constexpr size_t kMaxCommandPayloadSize = kCommandEnvelopeSize + 8;

struct JoinIdentity {
    uint8_t uid[protocol::kDeviceUidSize]{};
    uint64_t requestNonce = 0;
};
struct JoinRequest {
    JoinIdentity identity;
    uint16_t profileId = 0;
    protocol::FirmwareVersion firmware{};
    uint8_t maxPowerLevel = 0;
};
struct JoinAccept {
    JoinIdentity identity;
    uint8_t salt[kSaltSize]{};
    uint8_t nodeId = 0;
    uint8_t networkId = 0;
};

// These functions authenticate before decoding and leave output unchanged on failure.
bool sealJoinRequest(const osk::crypto::Cmac& factoryMac, Transport transport,
                     const JoinRequest& value, uint8_t* output, size_t capacity);
bool openJoinRequest(const osk::crypto::Cmac& factoryMac, Transport transport,
                     const uint8_t* wire, size_t size, JoinRequest& value);
bool sealJoinAccept(const osk::crypto::Cmac& factoryMac, Transport transport,
                    const JoinAccept& value, uint8_t* output, size_t capacity);
bool openJoinAccept(const osk::crypto::Cmac& factoryMac, Transport transport,
                    const uint8_t* wire, size_t size, JoinAccept& value);
// Salt participates in the MAC but is not transmitted in confirm or complete.
bool sealJoinProof(const osk::crypto::Cmac& sessionMac, Transport transport,
                   const JoinIdentity& value, const uint8_t (&salt)[kSaltSize],
                   bool complete, uint8_t* output, size_t capacity);
bool openJoinProof(const osk::crypto::Cmac& sessionMac, Transport transport,
                   const uint8_t* wire, size_t size, const uint8_t (&salt)[kSaltSize],
                   bool complete, JoinIdentity& value);

struct Command {
    uint16_t commandId = 0;
    uint8_t type = 0;
    uint8_t argumentSize = 0;
    uint8_t arguments[protocol::kMaxCommandArgumentSize]{};
};
struct CommandResult {
    uint16_t commandId = 0;
    protocol::CommandStatus status = protocol::CommandStatus::Applied;
    uint8_t dataSize = 0;
    uint8_t data[protocol::kMaxCommandResultDataSize]{};
};

// Payloads only: the authenticated frame counter binds the session.
// Command ready and No command have empty payloads.
bool encodeCommand(const Command& value, uint8_t* output, size_t capacity, size_t& size);
bool decodeCommand(const uint8_t* payload, size_t size, Command& value);
bool encodeCommandResult(const CommandResult& value, uint8_t* output, size_t capacity, size_t& size);
bool decodeCommandResult(const uint8_t* payload, size_t size, CommandResult& value);
// Accepts only a reply to the current Command ready, including a valid old tag.
bool openCommandReply(const Context& context, uint8_t nodeId, Transport transport,
                      uint32_t readyCounter, uint8_t* wire, size_t size,
                      Command& command, bool& hasCommand);

}  // namespace frames
}  // namespace security
}  // namespace radiosensors
