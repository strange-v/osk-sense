#pragma once
#include "ArduinoEepromStorage.h"
#include "NodeRadio.h"
#include "RadioSecurityPairing.h"
#include "UserRowStorage.h"

namespace radiosensors { namespace node {
enum class StartStatus : uint8_t { Ready, NoCredentials, RadioFailed, StorageFailed };

class CommissioningService {
public:
    CommissioningService(NodeRadio& radio, uint16_t profileId);
    StartStatus begin();
    bool active() const { return pairing_.active(); }
    bool provisional() const { return pairing_.provisional(); }
    bool advance();
    bool resetNetwork();
    const security_storage::NetworkConfig& config() const { return pairing_.config(); }
    const security::Context& context() const { return context_; }
    bool needsActivation() const { return challenged_; }
    bool sealFrame(uint8_t header, const uint8_t* payload, size_t payloadSize,
                   uint8_t control, uint8_t* output, size_t capacity, size_t& size, uint32_t& counter);
    // Input is the profile's telemetry serialization; only its payload goes on air.
    bool sendReport(const uint8_t* telemetry, size_t size, uint8_t attempts,
                    security::Ack& ack, int8_t& rssi);
    bool sendResult(const uint8_t* payload, size_t size);
    // Authenticated ACKs may change the floor or pending activation challenge.
    bool receiveAck(uint32_t counter, security::Transport transport,
                    const uint8_t* wire, size_t size, security::Ack& ack);
private:
    bool requestJoin();
    bool confirmJoin();
    bool exchange(uint8_t header, const uint8_t* payload, size_t size, uint8_t attempts,
                  security::Ack& ack, int8_t& rssi);
    bool applyAck(uint32_t counter, const security::Ack& ack);
    NodeRadio& radio_;
    storage::ArduinoEepromStorage eeprom_;
    storage::UserRowStorage userRow_;
    storage::FactoryCredentialStore<storage::UserRowStorage> factoryStore_;
    security_pairing::Transaction<storage::ArduinoEepromStorage> pairing_;
    security_storage::FrameCounterStore<storage::ArduinoEepromStorage> counters_;
    security::Context context_{};
    uint16_t profileId_;
    storage::FactoryCredentials factory_{};
    uint8_t challenge_[8]{};
    bool credentials_ = false, challenged_ = false;
};
} }
