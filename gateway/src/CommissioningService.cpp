#include "CommissioningService.h"

#include <RadioSecurityFrames.h>
#include <JoinRequest.h>

#include <atomic>

#include "GatewayStatus.h"
#include "ConfigurationStore.h"
#include "NodeRegistryStore.h"
#include "RadioConfig.h"
#include "RadioService.h"
#include "RecoveryService.h"

namespace gateway::commissioning {
namespace {

// Registry transaction snapshots and persistence workspaces stay off this task
// stack, leaving it for radio frames and commissioning control flow.
constexpr uint32_t kTaskStackSize = 8192;
constexpr UBaseType_t kTaskPriority = 6;
constexpr BaseType_t kTaskCore = 1;
constexpr uint32_t kConfirmTimeoutMs = 5000;

Snapshot counters{};
portMUX_TYPE countersMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> awaitingConfirm{false};
std::atomic<uint32_t> confirmDeadline{0};
uint8_t expectedDeviceUid[radiosensors::protocol::kDeviceUidSize]{};
bool expectedDeviceUidPresent = false;
osk::crypto::Cmac factoryMac;
portMUX_TYPE transactionMux = portMUX_INITIALIZER_UNLOCKED;

bool matchesExpectedDevice(const uint8_t* deviceUid) {
    portENTER_CRITICAL(&transactionMux);
    const bool matches = expectedDeviceUidPresent &&
        memcmp(expectedDeviceUid, deviceUid, sizeof(expectedDeviceUid)) == 0;
    portEXIT_CRITICAL(&transactionMux);
    return matches;
}

void setExpectedDevice(const uint8_t* deviceUid, const uint8_t* key) {
    osk::crypto::Cmac mac; osk::crypto::cmacInit(mac, key);
    portENTER_CRITICAL(&transactionMux);
    memcpy(expectedDeviceUid, deviceUid, sizeof(expectedDeviceUid));
    expectedDeviceUidPresent = true;
    factoryMac = mac;
    portEXIT_CRITICAL(&transactionMux);
    memset(&mac, 0, sizeof(mac));
}

void clearExpectedDevice(bool onlyExpired = false) {
    portENTER_CRITICAL(&transactionMux);
    if (onlyExpired && status::pairingActive()) {
        portEXIT_CRITICAL(&transactionMux);
        return;
    }
    memset(expectedDeviceUid, 0, sizeof(expectedDeviceUid));
    expectedDeviceUidPresent = false;
    memset(&factoryMac, 0, sizeof(factoryMac));
    portEXIT_CRITICAL(&transactionMux);
}

void increment(uint32_t Snapshot::*field) {
    portENTER_CRITICAL(&countersMux);
    ++(counters.*field);
    portEXIT_CRITICAL(&countersMux);
}

void returnToPairingIfOpen() {
    awaitingConfirm = false;
    confirmDeadline = 0;
    if (status::pairingActive()) {
        if (radio::requestProfile(radio::Profile::Commissioning)) {
            status::indicate(status::Indication::Pairing);
        } else {
            status::indicate(status::Indication::Error, 3000);
            Serial.println("Pairing recovery failed: radio profile did not switch");
        }
    }
}

void handleJoinRequest(const radio::ReceivedFrame& frame) {
    namespace s = radiosensors::security;
    namespace t = s::pairing;
    recovery::Guard windowGuard;
    if (!windowGuard || recovery::blocked()) return;
    increment(&Snapshot::joinRequests);
    if (!status::pairingActive() || frame.senderId != 0 || awaitingConfirm) {
        increment(&Snapshot::rejectedFrames);
        return;
    }
    osk::crypto::Cmac mac; uint8_t expectedUid[radiosensors::protocol::kDeviceUidSize];
    portENTER_CRITICAL(&transactionMux);
    const bool present = expectedDeviceUidPresent;
    if (present) {
        mac = factoryMac;
        memcpy(expectedUid, expectedDeviceUid, sizeof(expectedUid));
    }
    portEXIT_CRITICAL(&transactionMux);
    if (!present) { increment(&Snapshot::rejectedFrames); return; }
    const auto secrets = configuration_store::secrets();
    uint8_t encoded[s::frames::kJoinAcceptSize];
    status::indicate(status::Indication::PersistingNode);
    const auto result = registry_store::pairingRequestAndSave(
        mac, expectedUid,
        {static_cast<uint8_t>(frame.targetId),0,frame.control},
        frame.data, frame.size, secrets.operationalNetworkId, {0,100,0},
        encoded, sizeof(encoded));
    memset(&mac, 0, sizeof(mac));
    if (result != t::Status::Accept && result != t::Status::RepeatedAccept) {
        increment(result == t::Status::StorageError ? &Snapshot::storageErrors : &Snapshot::rejectedFrames);
        status::indicate(status::Indication::Error, 3000);
        return;
    }
    if (!status::pairingActive() || !matchesExpectedDevice(expectedUid) ||
        !radio::sendThenSwitchProfile(0, encoded, sizeof(encoded), radio::Profile::Operational)) {
        increment(&Snapshot::rejectedFrames);
        return;
    }
    increment(&Snapshot::joinAcceptsQueued);
    awaitingConfirm = true;
    confirmDeadline = millis() + kConfirmTimeoutMs;
    status::indicate(status::Indication::AwaitingConfirm);
}

void handleJoinConfirm(const radio::ReceivedFrame& frame) {
    namespace t = radiosensors::security::pairing;
    recovery::Guard windowGuard;
    if (!windowGuard || recovery::blocked()) return;
    increment(&Snapshot::joinConfirms);
    uint8_t encoded[radiosensors::security::frames::kJoinCompleteSize];
    const auto result = registry_store::pairingConfirmAndSave(
        static_cast<uint8_t>(frame.senderId),
        {static_cast<uint8_t>(frame.targetId),static_cast<uint8_t>(frame.senderId),frame.control},
        frame.data, frame.size, encoded, sizeof(encoded));
    const bool newlyConfirmed = result == t::Status::Complete;
    if (!newlyConfirmed && result != t::Status::RepeatedComplete) {
        increment(result == t::Status::StorageError ? &Snapshot::storageErrors : &Snapshot::rejectedFrames);
        status::indicate(status::Indication::Error, 3000);
        return;
    }
    if (!radio::send(static_cast<uint8_t>(frame.senderId), encoded, sizeof(encoded))) {
        increment(&Snapshot::rejectedFrames);
        return;
    }
    increment(&Snapshot::joinCompletesQueued);
    if (newlyConfirmed) increment(&Snapshot::nodesActivated);
    if (matchesExpectedDevice(encoded + 1)) {
        awaitingConfirm = false; confirmDeadline = 0;
        if (status::closePairing()) {
            clearExpectedDevice();
            status::indicate(status::Indication::PairingSucceeded, 1000);
        }
    }
}

void task(void*) {
    for (;;) {
        radio::ReceivedFrame received{};
        if (radio::receive(received, pdMS_TO_TICKS(100))) {
            if (received.kind == radiosensors::protocol::FrameKind::JoinRequest) {
                handleJoinRequest(received);
            } else if (received.kind == radiosensors::protocol::FrameKind::JoinConfirm) {
                handleJoinConfirm(received);
            }
        }
        clearExpectedDevice(true);
        if (awaitingConfirm &&
            static_cast<int32_t>(millis() - confirmDeadline) >= 0) {
            increment(&Snapshot::confirmTimeouts);
            returnToPairingIfOpen();
        }
    }
}

}  // namespace

bool begin() {
    if (xTaskCreatePinnedToCore(
            task, "commissioning", kTaskStackSize, nullptr, kTaskPriority,
            nullptr, kTaskCore) != pdPASS) {
        Serial.println("Commissioning task creation failed");
        return false;
    }
    Serial.println("Commissioning service ready");
    return true;
}

bool open(
    const uint8_t deviceUid[radiosensors::protocol::kDeviceUidSize],
    const uint8_t factoryKey[radiosensors::gateway_storage::kRadioKeySize]) {
    if (deviceUid == nullptr || factoryKey == nullptr) return false;
    awaitingConfirm = false;
    confirmDeadline = 0;
    if (!status::openPairing()) return false;
    setExpectedDevice(deviceUid, factoryKey);
    if (!radio::beginCommissioning()) {
        status::closePairing();
        clearExpectedDevice();
        return false;
    }
    return true;
}

bool close() {
    const bool result = status::closePairing();
    clearExpectedDevice();
    return result;
}

Snapshot snapshot() {
    portENTER_CRITICAL(&countersMux);
    const Snapshot value = counters;
    portEXIT_CRITICAL(&countersMux);
    return value;
}

}  // namespace gateway::commissioning
