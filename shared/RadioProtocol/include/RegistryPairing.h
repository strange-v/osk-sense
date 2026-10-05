#pragma once

#include "RegistryPersistence.h"

namespace radiosensors {
namespace registry {

// Caller serializes registry and replay-store access. The transport must send
// only responses returned with Accept/RepeatedAccept or Complete/RepeatedComplete.
class PairingAdapter {
public:
    PairingAdapter(NodeRegistry& nodes, AtomicRegistryStore& store,
                   replay::Store& bounds, replay::Guard& guard, replay::RandomSource& random)
        : nodes_(nodes), store_(store), bounds_(bounds), guard_(guard), random_(random) {}
    security::pairing::Status request(
        const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
        security::Transport transport, const uint8_t* wire, size_t size,
        uint8_t networkId, security::Transport acceptTransport,
        uint8_t* output, size_t capacity);
    security::pairing::Status confirm(
        uint8_t nodeId, security::Transport transport, const uint8_t* wire,
        size_t size, uint8_t* output, size_t capacity);
    bool activeKeys(uint8_t nodeId, security::Keys& keys, uint8_t& replaySlot) const;
    bool activeSalt(uint8_t nodeId, uint8_t* salt) const;
private:
    NodeRegistry& nodes_;
    AtomicRegistryStore& store_;
    replay::Store& bounds_;
    replay::Guard& guard_;
    replay::RandomSource& random_;
};

}  // namespace registry
}  // namespace radiosensors
