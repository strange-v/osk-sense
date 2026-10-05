#pragma once

#include "RadioSecurityStorage.h"
#include <RadioSecurityFrames.h>

namespace radiosensors {
namespace node {
namespace security_pairing {

enum class Status : uint8_t { Accepted, Pinned, Active, InvalidFrame, StorageError };

template <typename Storage>
class Transaction {
public:
    explicit Transaction(Storage& storage) : store_(storage) {}
    Transaction(Storage& storage, const uint8_t (&uid)[protocol::kDeviceUidSize]) : store_(storage) {
        memcpy(identity_.uid,uid,sizeof(identity_.uid));
    }
    bool load() {
        config_ = security_storage::NetworkConfig{};
        identity_.requestNonce = 0;
        hasConfig_ = store_.load(config_);
        attempting_ = false;
        healthy_ = true;
        if (hasConfig_) identity_.requestNonce = config_.requestNonce;
        return hasConfig_;
    }
    bool load(const uint8_t (&uid)[protocol::kDeviceUidSize]) {
        memcpy(identity_.uid,uid,sizeof(identity_.uid)); return load();
    }
    bool reset() {
        if (!store_.factoryReset()) { healthy_ = false; return false; }
        config_ = security_storage::NetworkConfig{};
        identity_.requestNonce = 0;
        hasConfig_ = attempting_ = false; healthy_ = true;
        return true;
    }
    bool active() const { return healthy_ && hasConfig_ && config_.state == storage::ProvisioningState::Active; }
    bool provisional() const { return healthy_ && hasConfig_ && config_.state == storage::ProvisioningState::Provisional; }
    // The caller allocates the counter and qualifies the entropy before begin.
    bool begin(uint64_t nonce) {
        if (!healthy_ || hasConfig_) return false;
        identity_.requestNonce = nonce; attempting_ = true; return true;
    }
    const security_storage::NetworkConfig& config() const { return config_; }
    const security::frames::JoinIdentity& identity() const { return identity_; }
    Status accept(const osk::crypto::Cmac& factoryMac, security::Transport transport,
                  const uint8_t* wire, size_t size) {
        if (!healthy_) return Status::StorageError;
        if (hasConfig_) return Status::Pinned;
        security::frames::JoinAccept value;
        if (!attempting_ || transport.target != 0 || transport.sender != 100 ||
            !security::frames::openJoinAccept(factoryMac,transport,wire,size,value) ||
            memcmp(value.identity.uid,identity_.uid,sizeof(identity_.uid)) != 0 ||
            value.identity.requestNonce != identity_.requestNonce) return Status::InvalidFrame;
        security_storage::NetworkConfig candidate;
        candidate.nodeId = value.nodeId; candidate.gatewayId = 100;
        candidate.networkId = value.networkId; candidate.requestNonce = value.identity.requestNonce;
        memcpy(candidate.salt,value.salt,sizeof(candidate.salt));
        if (!store_.save(candidate)) { healthy_ = false; return Status::StorageError; }
        config_ = candidate; hasConfig_ = true; attempting_ = false;
        return Status::Accepted;
    }
    // sessionMac must be derived from config().salt. No confirm precedes readback.
    bool confirm(const osk::crypto::Cmac& sessionMac, uint8_t* out, size_t capacity) const {
        return healthy_ && hasConfig_ && config_.state == storage::ProvisioningState::Provisional &&
            security::frames::sealJoinProof(sessionMac,security::Transport{100,config_.nodeId,0},
                                           identity_,config_.salt,false,out,capacity);
    }
    Status complete(const osk::crypto::Cmac& sessionMac, security::Transport transport,
                    const uint8_t* wire, size_t size) {
        if (!healthy_) return Status::StorageError;
        security::frames::JoinIdentity proof;
        if (!hasConfig_ || transport.target != config_.nodeId || transport.sender != 100 ||
            !security::frames::openJoinProof(sessionMac,transport,wire,size,config_.salt,true,proof) ||
            memcmp(proof.uid,identity_.uid,sizeof(identity_.uid)) != 0 ||
            proof.requestNonce != identity_.requestNonce) return Status::InvalidFrame;
        if (config_.state == storage::ProvisioningState::Active) return Status::Active;
        auto candidate = config_; candidate.state = storage::ProvisioningState::Active;
        if (!store_.save(candidate)) { healthy_ = false; return Status::StorageError; }
        config_ = candidate; return Status::Active;
    }
private:
    security_storage::NetworkConfigStore<Storage> store_;
    security_storage::NetworkConfig config_{};
    security::frames::JoinIdentity identity_{};
    bool healthy_ = false, hasConfig_ = false, attempting_ = false;
};

}  // namespace security_pairing
}  // namespace node
}  // namespace radiosensors
