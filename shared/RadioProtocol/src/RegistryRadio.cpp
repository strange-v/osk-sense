#include "RegistryRadio.h"

#ifndef __AVR__
#include "TelemetryFrames.h"
#include <new>
#include <string.h>

namespace radiosensors {
namespace registry {
namespace s = security;
namespace f = s::frames;

void RadioAdapter::restart() {
    for (auto& history : histories_) history.frames.reset();
}

ReceiveStatus RadioAdapter::receive(s::Transport transport, uint8_t* wire, size_t size,
                                    uint64_t now, bool telemetrySpace, bool sessionSpace,
                                    OpenedFrame& frame) {
    frame.payloadSize = frame.replySize = 0;
    if (!wire || size == 0 || size > s::kMaxWireSize || transport.target != 100 ||
        transport.sender < kFirstNodeId || transport.sender > kLastNodeId ||
        (transport.control != 0 && transport.control != 0x40)) return ReceiveStatus::InvalidFrame;
    s::Keys keys; uint8_t slot = 255;
    if (!pairing_.activeKeys(transport.sender, keys, slot)) return ReceiveStatus::UnknownNode;
    s::initialize(context_, keys);
    uint8_t* payload = nullptr; size_t payloadSize = 0; uint32_t counter = 0;
    const uint8_t header = wire[0];
    uint8_t challenge[s::kSaltSize];
    if (header == s::kActivationHeader && size >= 5 + sizeof(challenge))
        memcpy(challenge, wire + 5, sizeof(challenge));
    if (!s::open(context_, s::Direction::Node, transport.sender, transport,
                 wire, size, counter, payload, payloadSize)) return ReceiveStatus::FailedTag;
    const bool telemetry = header == 0x60 || header == s::kActivationHeader;
    if (telemetry) {
        if (payloadSize < protocol::kTelemetryPrefixSize - 1 ||
            (payload[0] & protocol::kRadioStateReservedMask) != 0) return ReceiveStatus::InvalidFrame;
    } else if (header == f::kCommandReadyHeader) {
        if (payloadSize != 0) return ReceiveStatus::InvalidFrame;
    } else if (header == f::kCommandResultHeader) {
        f::CommandResult result;
        if (!f::decodeCommandResult(payload, payloadSize, result)) return ReceiveStatus::InvalidFrame;
    } else return ReceiveStatus::InvalidFrame;

    // A full consumer queue must not consume a fresh counter. A cached reply
    // needs no queue entry and can still answer a repeated Command ready.
    size_t cachedSize = 0;
    const bool cached = header == f::kCommandReadyHeader &&
        guard_.cachedReply(slot, counter, frame.reply, sizeof(frame.reply), cachedSize);
    if (!(telemetry ? telemetrySpace : sessionSpace) && !cached) return ReceiveStatus::Busy;
    auto& history = histories_[slot];
    // Obtain the public salt from the transaction, not from untrusted radio data.
    // PairingAdapter exposes it alongside the keys to bind queued work to them.
    if (!pairing_.activeSalt(transport.sender, frame.salt)) return ReceiveStatus::UnknownNode;
    if (!history.frames || memcmp(history.salt, frame.salt, sizeof(history.salt)) != 0) {
        history.frames.reset(new (std::nothrow) replay::AcceptanceHistory);
        if (!history.frames) return ReceiveStatus::StorageError;
        memcpy(history.salt, frame.salt, sizeof(history.salt));
        history.frames->begin(now);
    }
    frame.decision = guard_.inspect(slot, counter, history.frames->window(now),
                                   header == s::kActivationHeader ? challenge : nullptr);
    if (frame.decision.action == replay::Action::StorageError) return ReceiveStatus::StorageError;
    frame.header = header; frame.counter = counter; frame.mac = context_.authentication;
    memcpy(frame.payload, payload, payloadSize); frame.payloadSize = payloadSize;
    if (frame.decision.action == replay::Action::Accept) {
        history.frames->accepted(now);
    } else if (frame.decision.action == replay::Action::Duplicate && cached) {
        frame.replySize = cachedSize;
    }
    return ReceiveStatus::Ok;
}

bool RadioAdapter::reply(uint8_t nodeId, const uint8_t* salt, uint32_t counter,
                         uint8_t header, const uint8_t* payload, size_t size,
                         uint8_t* output, size_t capacity, size_t& outputSize) {
    outputSize = 0;
    if (!salt || !output || (header != f::kCommandHeader && header != f::kNoCommandHeader)) return false;
    s::Keys keys; uint8_t slot = 255, currentSalt[s::kSaltSize];
    if (!pairing_.activeKeys(nodeId, keys, slot) || !pairing_.activeSalt(nodeId, currentSalt) ||
        memcmp(salt, currentSalt, sizeof(currentSalt)) != 0) return false;
    if (guard_.cachedReply(slot, counter, output, capacity, outputSize)) return true;
    if (header == f::kNoCommandHeader) {
        if (size != 0) return false;
    } else {
        f::Command command;
        if (!f::decodeCommand(payload, size, command)) return false;
    }
    s::initialize(context_, keys);
    uint8_t sealed[replay::kMaxReplySize]; size_t sealedSize = 0;
    if (!s::seal(context_, s::Direction::Gateway, nodeId, {nodeId,100,0}, counter,
                 header, payload, size, sealed, sizeof(sealed), sealedSize) ||
        capacity < sealedSize || !guard_.cacheReply(slot, counter, sealed, sealedSize)) return false;
    memcpy(output, sealed, sealedSize); outputSize = sealedSize;
    return true;
}

}  // namespace registry
}  // namespace radiosensors
#endif
