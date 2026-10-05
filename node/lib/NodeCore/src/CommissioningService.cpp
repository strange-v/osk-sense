#include "CommissioningService.h"
#include "NodeFirmware.h"
#include "PairingEntropy.h"
#include "RtcEntropy.h"
#include <Arduino.h>
#include <string.h>

namespace radiosensors { namespace node {
namespace f = security::frames;
CommissioningService::CommissioningService(NodeRadio& radio, uint16_t profileId)
    : radio_(radio), factoryStore_(userRow_), pairing_(eeprom_), counters_(eeprom_), profileId_(profileId) {}

StartStatus CommissioningService::begin() {
    uint8_t uid[protocol::kDeviceUidSize];
    volatile const uint8_t* serial = &SIGROW_SERNUM0;
    for (uint8_t i = 0; i < sizeof(uid); ++i) uid[i] = serial[i];
    bool configured = pairing_.load(uid);
    credentials_ = factoryStore_.load(factory_);
    if (!credentials_) {
        if (radio_.begin(0,0)) radio_.sleep();
        return StartStatus::NoCredentials;
    }
    if (counters_.load(configured) == security_storage::CounterLoadStatus::KeysMustBeDiscarded) {
        if (!pairing_.reset()) return StartStatus::StorageFailed;
        counters_.load(false); configured = false;
    }
    if (!radio_.begin(configured ? config().nodeId : 0, configured ? config().networkId : 0))
        return StartStatus::RadioFailed;
    if (configured) {
        security::initialize(context_,factory_.key,config().salt);
        radio_.useProfile(config().nodeId,config().networkId);
    } else radio_.useProfile(0,0);
    radio_.sleep(); return StartStatus::Ready;
}
bool CommissioningService::resetNetwork() {
    if (!credentials_) return false;
    return pairing_.reset();
}
bool CommissioningService::advance() {
    if (active()) return true;
    if (!credentials_ || !radio_.ensureConfigured()) return false;
    return provisional() ? confirmJoin() : requestJoin();
}
bool CommissioningService::requestJoin() {
    radio_.useProfile(0,0); radio_.sleep();
    uint8_t material[8]; uint32_t counter;
    if (!collectRtcEntropy(material) || !counters_.take(counter)) return false;
    osk::crypto::cmacInit(context_.authentication,factory_.key);
    const uint32_t entropy = pairing_entropy::condition(context_.authentication,pairing_.identity().uid,counter,material);
    if (!pairing_.begin((static_cast<uint64_t>(entropy) << 32) | counter)) return false;
    f::JoinRequest request;
    request.identity = pairing_.identity(); request.profileId = profileId_;
    request.firmware = kFirmwareVersion; request.maxPowerLevel = NODE_RADIO_MAX_POWER_LEVEL;
    uint8_t wire[f::kJoinAcceptSize];
    if (!f::sealJoinRequest(context_.authentication,security::Transport{100,0,0},request,wire,sizeof(wire))) return false;
    radio_.send(100,wire,f::kJoinRequestSize);
    const uint32_t started = millis();
    for (;;) {
        const uint32_t elapsed = static_cast<uint32_t>(millis() - started);
        if (elapsed >= 2500) break;
        const uint32_t remaining = 2500 - elapsed;
        const uint8_t size = radio_.receiveFrame(remaining,100,wire,sizeof(wire));
        if (!size) break;
        const auto status = pairing_.accept(context_.authentication,radio_.receivedTransport(),wire,size);
        if (status == security_pairing::Status::StorageError) break;
        if (status != security_pairing::Status::Accepted) continue;
        security::initialize(context_,factory_.key,config().salt);
        radio_.useProfile(config().nodeId,config().networkId);
        return confirmJoin();
    }
    radio_.sleep(); return false;
}
bool CommissioningService::confirmJoin() {
    uint8_t confirm[f::kJoinConfirmSize], complete[f::kJoinCompleteSize];
    if (!pairing_.confirm(context_.authentication,confirm,sizeof(confirm))) return false;
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        radio_.send(100,confirm,sizeof(confirm));
        const uint32_t started = millis();
        for (;;) {
            const uint32_t elapsed = static_cast<uint32_t>(millis() - started);
            if (elapsed >= 1000) break;
            const uint32_t remaining = 1000 - elapsed;
            const uint8_t size = radio_.receiveFrame(remaining,100,complete,sizeof(complete));
            if (!size) break;
            const auto status = pairing_.complete(context_.authentication,radio_.receivedTransport(),complete,size);
            if (status == security_pairing::Status::StorageError) { radio_.sleep(); return false; }
            if (status == security_pairing::Status::Active) { radio_.sleep(); return true; }
        }
    }
    radio_.sleep(); return false;
}
bool CommissioningService::sealFrame(uint8_t header, const uint8_t* payload, size_t payloadSize,
    uint8_t control, uint8_t* output, size_t capacity, size_t& size, uint32_t& counter) {
    if (!active() || !counters_.take(counter)) return false;
    return security::seal(context_,security::Direction::Node,config().nodeId,
        security::Transport{100,config().nodeId,control},counter,header,payload,payloadSize,output,capacity,size,
        header == security::kActivationHeader ? challenge_ : nullptr);
}
bool CommissioningService::applyAck(uint32_t counter, const security::Ack& ack) {
    if (ack.hasCounterFloor && !counters_.advanceToFloor(counter,ack.counterFloor)) return false;
    if (ack.hasChallenge) { memcpy(challenge_,ack.challenge,sizeof(challenge_)); challenged_ = true; }
    return true;
}
bool CommissioningService::receiveAck(uint32_t counter, security::Transport transport,
    const uint8_t* wire, size_t size, security::Ack& ack) {
    return transport.sender == 100 && transport.target == config().nodeId && transport.control == 0x80 &&
        security::openAck(context_.authentication,transport,counter,wire,size,ack) && applyAck(counter,ack);
}
bool CommissioningService::exchange(uint8_t header, const uint8_t* payload, size_t payloadSize,
    uint8_t attempts, security::Ack& ack, int8_t& rssi) {
    if (!radio_.ensureConfigured()) return false;
    for (uint8_t recovery = 0; recovery < 3; ++recovery) {
        uint8_t wire[security::kMaxWireSize]; size_t size; uint32_t counter;
        const uint8_t kind = header == 0x60 && challenged_ ? security::kActivationHeader : header;
        if (!sealFrame(kind,payload,payloadSize,0x40,wire,sizeof(wire),size,counter) ||
            !radio_.sendAcknowledged(100,wire,static_cast<uint8_t>(size),attempts,
                                     context_.authentication,counter,ack,rssi)) return false;
        if (!applyAck(counter,ack)) return false;
        if (ack.hasCounterFloor) continue;
        if (ack.hasChallenge) {
            if (header == 0x60) continue;
            return false;
        }
        if (header == 0x60) challenged_ = false;
        return true;
    }
    return false;
}
bool CommissioningService::sendReport(const uint8_t* telemetry, size_t size, uint8_t attempts,
    security::Ack& ack, int8_t& rssi) {
    return telemetry && size > 1 && exchange(0x60,telemetry + 1,size - 1,attempts,ack,rssi);
}
bool CommissioningService::sendResult(const uint8_t* payload, size_t size) {
    security::Ack ack; int8_t rssi;
    return exchange(f::kCommandResultHeader,payload,size,3,ack,rssi);
}
} }
