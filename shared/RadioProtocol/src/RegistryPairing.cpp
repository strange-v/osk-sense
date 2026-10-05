#include "RegistryPairing.h"

#ifndef __AVR__
#include <memory>
#include <new>
#include <string.h>

namespace radiosensors {
namespace registry {
namespace {
namespace t = security::pairing;

class TransactionStorage final : public replay::BlobStorage {
public:
    TransactionStorage(NodeRegistry& nodes, AtomicRegistryStore& store,
                       replay::Store& bounds, replay::Guard& guard,
                       const uint8_t* uid, uint8_t slot)
        : nodes_(nodes), store_(store), bounds_(bounds), guard_(guard), slot_(slot) {
        memcpy(uid_, uid, sizeof(uid_));
    }
    replay::ReadStatus read(uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (!store_.writable()) return replay::ReadStatus::Error;
        const auto* record = nodes_.findByUid(uid_);
        if (!record || record->replaySlot == 255) return replay::ReadStatus::Missing;
        if (!output || capacity < sizeof(record->pairing)) return replay::ReadStatus::Error;
        memcpy(output, record->pairing, sizeof(record->pairing));
        size = sizeof(record->pairing);
        return replay::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        t::Record pairing;
        uint32_t generation = 0;
        if (!store_.writable() || !t::decode(data, size, pairing, generation) ||
            pairing.state == t::State::Empty || memcmp(pairing.request + 1, uid_, sizeof(uid_)) != 0)
            return false;
        std::unique_ptr<NodeRegistry> candidate(new (std::nothrow) NodeRegistry(nodes_));
        if (!candidate || !candidate->setPairing(pairing, generation, slot_)) return false;
        // New Pending keys cannot inherit a bound from a deleted node or an
        // interrupted confirmation. Forget before making the new keys durable.
        if (pairing.state == t::State::Pending &&
            (!bounds_.writable() || !guard_.forget(slot_))) return false;
        if (!store_.save(*candidate)) return false;
        nodes_ = *candidate;
        return true;
    }
private:
    NodeRegistry& nodes_;
    AtomicRegistryStore& store_;
    replay::Store& bounds_;
    replay::Guard& guard_;
    uint8_t uid_[protocol::kDeviceUidSize];
    uint8_t slot_;
};

class BoundInitializer final : public t::BoundInitializer {
public:
    BoundInitializer(const NodeRegistry& nodes, replay::Store& bounds,
                     replay::Guard& guard, uint8_t nodeId)
        : nodes_(nodes), bounds_(bounds), guard_(guard), nodeId_(nodeId) {}
    bool ensure(const security::Keys& keys) override {
        const auto* node = nodes_.findByNodeId(nodeId_);
        t::Record pairing;
        uint32_t generation = 0;
        if (!node || node->state != NodeState::Pending || node->replaySlot >= replay::kNodeSlots ||
            !bounds_.writable() || !t::decode(node->pairing, sizeof(node->pairing), pairing, generation) ||
            pairing.state != t::State::Pending ||
            memcmp(keys.encryption, pairing.keys.encryption, sizeof(keys.encryption)) != 0 ||
            memcmp(keys.authentication, pairing.keys.authentication, sizeof(keys.authentication)) != 0)
            return false;
        const auto state = bounds_.snapshot().records[node->replaySlot].state;
        if (state == replay::RecordState::Paired) return true;
        return state == replay::RecordState::Absent && guard_.initializeFreshKeys(node->replaySlot);
    }
private:
    const NodeRegistry& nodes_;
    replay::Store& bounds_;
    replay::Guard& guard_;
    uint8_t nodeId_;
};
}

security::pairing::Status PairingAdapter::request(
    const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
    security::Transport transport, const uint8_t* wire, size_t size,
    uint8_t networkId, security::Transport acceptTransport,
    uint8_t* output, size_t capacity) {
    if (!store_.writable()) return t::Status::StorageError;
    security::frames::JoinRequest request;
    if (!security::frames::openJoinRequest(factoryMac, transport, wire, size, request))
        return t::Status::InvalidFrame;
    if (!expectedUid || memcmp(expectedUid, request.identity.uid, protocol::kDeviceUidSize) != 0)
        return t::Status::WrongUid;
    const auto* node = nodes_.findByUid(expectedUid);
    if (node && node->state == NodeState::Disabled) return t::Status::DisabledNode;
    if (node && node->state == NodeState::Active) return t::Status::ActiveNode;
    uint8_t nodeId = node ? node->nodeId : 0;
    if (!nodeId) {
        for (uint8_t id = kFirstNodeId; id <= kLastNodeId; ++id)
            if (!nodes_.findByNodeId(id)) { nodeId = id; break; }
    }
    uint8_t slot = node ? node->replaySlot : 255;
    if (slot == 255) {
        for (uint8_t i = 0; i < replay::kNodeSlots; ++i) {
            bool used = false;
            for (size_t j = 0; j < nodes_.size(); ++j)
                if (nodes_.records()[j].replaySlot == i) { used = true; break; }
            if (!used) { slot = i; break; }
        }
    }
    if (!nodeId || slot == 255 || (!node && nodes_.size() >= kMaxNodes)) return t::Status::RegistryFull;
    TransactionStorage storage(nodes_, store_, bounds_, guard_, expectedUid, slot);
    t::Transaction transaction(storage, random_);
    if (!transaction.load()) return t::Status::StorageError;
    return transaction.request(factoryMac, expectedUid, transport, wire, size,
                               nodeId, networkId, acceptTransport, output, capacity);
}

security::pairing::Status PairingAdapter::confirm(
    uint8_t nodeId, security::Transport transport, const uint8_t* wire,
    size_t size, uint8_t* output, size_t capacity) {
    if (!store_.writable()) return t::Status::StorageError;
    const auto* node = nodes_.findByNodeId(nodeId);
    if (!node || node->replaySlot == 255 || transport.sender != nodeId) return t::Status::InvalidFrame;
    if (node->state == NodeState::Disabled) return t::Status::DisabledNode;
    TransactionStorage storage(nodes_, store_, bounds_, guard_, node->deviceUid, node->replaySlot);
    t::Transaction transaction(storage, random_);
    BoundInitializer bound(nodes_, bounds_, guard_, nodeId);
    if (!transaction.load()) return t::Status::StorageError;
    return transaction.confirm(transport, wire, size, bound, output, capacity);
}

bool PairingAdapter::activeKeys(uint8_t nodeId, security::Keys& keys, uint8_t& replaySlot) const {
    if (!store_.writable()) return false;
    const auto* node = nodes_.findByNodeId(nodeId);
    t::Record pairing;
    uint32_t generation = 0;
    if (!node || node->state != NodeState::Active || node->replaySlot == 255 ||
        !t::decode(node->pairing, sizeof(node->pairing), pairing, generation) ||
        pairing.state != t::State::Active) return false;
    keys = pairing.keys;
    replaySlot = node->replaySlot;
    return true;
}

bool PairingAdapter::activeSalt(uint8_t nodeId, uint8_t* salt) const {
    if (!store_.writable() || !salt) return false;
    const auto* node = nodes_.findByNodeId(nodeId);
    if (!node || node->state != NodeState::Active || node->replaySlot == 255) return false;
    t::Record pairing; uint32_t generation = 0;
    if (!t::decode(node->pairing, sizeof(node->pairing), pairing, generation) ||
        pairing.state != t::State::Active) return false;
    memcpy(salt, pairing.accept + 19, security::kSaltSize);
    return true;
}

}  // namespace registry
}  // namespace radiosensors
#endif
