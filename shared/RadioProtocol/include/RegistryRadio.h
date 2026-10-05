#pragma once

#include "RegistryPairing.h"
#ifndef __AVR__
#include <memory>

namespace radiosensors {
namespace registry {

enum class ReceiveStatus : uint8_t { Ok, InvalidFrame, FailedTag, UnknownNode, Busy, StorageError };
struct OpenedFrame {
    uint8_t header = 0;
    uint32_t counter = 0;
    uint8_t salt[security::kSaltSize]{};
    uint8_t payload[security::kMaxWireSize]{};
    size_t payloadSize = 0;
    replay::Decision decision;
    osk::crypto::Cmac mac;
    uint8_t reply[replay::kMaxReplySize]{};
    size_t replySize = 0;
};

// Caller holds the registry mutex. Only authenticated Accept frames reach
// consumers; all replies are bound to the counter and the current pairing salt.
class RadioAdapter {
public:
    RadioAdapter(PairingAdapter& pairing, replay::Guard& guard) : pairing_(pairing), guard_(guard) {}
    void restart();
    ReceiveStatus receive(security::Transport transport, uint8_t* wire, size_t size,
                          uint64_t uptimeSeconds, bool telemetrySpace, bool sessionSpace,
                          OpenedFrame& frame);
    bool reply(uint8_t nodeId, const uint8_t* salt, uint32_t readyCounter,
               uint8_t header, const uint8_t* payload, size_t size,
               uint8_t* output, size_t capacity, size_t& outputSize);
private:
    PairingAdapter& pairing_;
    replay::Guard& guard_;
    security::Context context_;
    struct History {
        std::unique_ptr<replay::AcceptanceHistory> frames;
        uint8_t salt[security::kSaltSize]{};
    } histories_[replay::kNodeSlots];
};

}  // namespace registry
}  // namespace radiosensors
#endif
