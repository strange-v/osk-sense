#include "NodeRegistryStore.h"

#include <Preferences.h>
#include <RegistryPersistence.h>
#include <RegistryPairing.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <atomic>
#include <memory>
#include <new>
#include <string.h>

#include "RecoveryService.h"
#include "ReplayBoundStorage.h"

namespace gateway::registry_store {
namespace {

constexpr const char* kNamespace = "node-reg";
constexpr const char* kKey = "registry";
constexpr TickType_t kRadioLockWait = pdMS_TO_TICKS(5);
static_assert(kRadioLockWait > 0, "radio lock wait requires a tick shorter than 5 ms");

TickType_t remainingRadioLockWait(const TickType_t started) {
    const TickType_t elapsed = xTaskGetTickCount() - started;
    return elapsed < kRadioLockWait ? kRadioLockWait - elapsed : 0;
}

class NvsRegistryStorage final : public radiosensors::replay::BlobStorage {
public:
    bool begin() {
        return preferences_.begin(kNamespace, false);
    }

    radiosensors::replay::ReadStatus read(
        uint8_t* const output,
        const size_t capacity,
        size_t& size) override {
        using radiosensors::replay::ReadStatus;
        size = 0;
        if (!preferences_.isKey(kKey)) return ReadStatus::Missing;
        const size_t storedSize = preferences_.getBytesLength(kKey);
        if (storedSize == 0 || storedSize > capacity) {
            return ReadStatus::Invalid;
        }
        const size_t bytesRead = preferences_.getBytes(
            kKey, output, storedSize);
        if (bytesRead != storedSize) {
            return ReadStatus::Error;
        }
        size = bytesRead;
        return ReadStatus::Ok;
    }

    bool write(
        const uint8_t* const data,
        const size_t size) override {
        return data != nullptr && preferences_.putBytes(kKey, data, size) == size;
    }

private:
    Preferences preferences_;
};

class RandomSource final : public radiosensors::replay::RandomSource {
public:
    bool fill(uint8_t* output, size_t size) override {
        if (!output) return false;
        esp_fill_random(output, size);
        return true;
    }
};

NvsRegistryStorage storage;
radiosensors::registry::NodeRegistry nodes;
radiosensors::registry::AtomicRegistryStore store(storage);
ReplayBoundStorage boundStorage;
radiosensors::replay::Store bounds(boundStorage);
RandomSource random;
radiosensors::replay::Guard replayGuard(bounds, random);
radiosensors::registry::PairingAdapter pairing(nodes, store, bounds, replayGuard, random);
radiosensors::registry::RadioAdapter secureRadio(pairing, replayGuard);
bool initialized = false;
SemaphoreHandle_t mutex = nullptr;
std::atomic<bool> reloadRequired{false};
std::atomic<uint32_t> nextReloadAt{0};
constexpr uint32_t kReloadIntervalMs = 1000;
std::atomic<uint32_t> activeNodeIds[4]{};
std::atomic<uint32_t> publishedGeneration{0};
std::atomic<uint32_t> lastReceiveUs{0}, maxReceiveUs{0};
std::atomic<uint32_t> historyBytes{0};

struct CommitTiming {
    bool saved;
    size_t records;
    uint64_t saveUs;
    uint64_t lockUs;
};

uint64_t commitClockUs() {
#if GATEWAY_REGISTRY_TIMING_LOG
    return esp_timer_get_time();
#else
    return 0;
#endif
}

// Remove bounds only after their registry records are durably gone. Also run
// after loading to finish cleanup interrupted by a reset or storage failure.
bool forgetUnusedReplaySlots() {
    if (!bounds.writable()) return false;
    bool used[radiosensors::replay::kNodeSlots]{};
    for (size_t i = 0; i < nodes.size(); ++i) {
        const uint8_t slot = nodes.records()[i].replaySlot;
        if (slot < radiosensors::replay::kNodeSlots) used[slot] = true;
    }
    for (size_t slot = 0; slot < radiosensors::replay::kNodeSlots; ++slot) {
        if (!used[slot] && bounds.snapshot().records[slot].state !=
            radiosensors::replay::RecordState::Absent && !replayGuard.forget(slot)) return false;
    }
    return true;
}

// The registry state readers may see without the mutex: the active-node bitmap
// the radio receive path tests per frame, and the durable generation. Call
// after every successful commit -- one function, so a later commit cannot
// republish half of it.
void publishLockFreeView() {
    secureRadio.retainActiveSlots(nodes);
    historyBytes.store(static_cast<uint32_t>(secureRadio.historyBytes()));
    if (!store.writable() || !bounds.writable()) {
        if (!reloadRequired.exchange(true)) nextReloadAt = millis() + kReloadIntervalMs;
    }
    uint32_t words[4]{};
    for (size_t index = 0; index < nodes.size(); ++index) {
        const radiosensors::registry::NodeRecord& record = nodes.records()[index];
        if (!reloadRequired.load() && record.state == radiosensors::registry::NodeState::Active &&
            record.nodeId <= radiosensors::registry::kLastNodeId) {
            words[record.nodeId / 32U] |= 1UL << (record.nodeId % 32U);
        }
    }
    for (size_t index = 0; index < 4; ++index) {
        activeNodeIds[index].store(words[index], std::memory_order_release);
    }
    publishedGeneration.store(store.generation(), std::memory_order_release);
}

// Call with the registry mutex held. Serial output happens after unlock so the
// diagnostic itself is not counted as telemetry-blocking time.
CommitTiming commitCandidateLocked(
    const radiosensors::registry::NodeRegistry& candidate,
    const uint64_t lockStartedUs) {
#if GATEWAY_REGISTRY_TIMING_LOG
    const uint64_t saveStartedUs = esp_timer_get_time();
    const bool saved = store.save(candidate);
    const uint64_t saveFinishedUs = esp_timer_get_time();
#else
    const bool saved = store.save(candidate);
#endif
    if (saved) {
        nodes = candidate;
    }
    publishLockFreeView();
#if GATEWAY_REGISTRY_TIMING_LOG
    return CommitTiming{
        saved,
        candidate.size(),
        saveFinishedUs - saveStartedUs,
        static_cast<uint64_t>(esp_timer_get_time()) - lockStartedUs,
    };
#else
    (void)lockStartedUs;
    return CommitTiming{saved, candidate.size(), 0, 0};
#endif
}

void logCommitTiming(const char* const operation, const CommitTiming& timing) {
#if GATEWAY_REGISTRY_TIMING_LOG
    Serial.printf(
        "Registry commit timing: operation=%s result=%s records=%u "
        "save_us=%llu lock_us=%llu\n",
        operation,
        timing.saved ? "ok" : "failed",
        static_cast<unsigned>(timing.records),
        static_cast<unsigned long long>(timing.saveUs),
        static_cast<unsigned long long>(timing.lockUs));
#else
    (void)operation;
    (void)timing;
#endif
}

}  // namespace

bool begin() {
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        Serial.println("Node registry mutex creation failed");
        return false;
    }
    if (!storage.begin()) {
        Serial.println("Node registry NVS initialization failed");
        return false;
    }

    const radiosensors::registry::LoadStatus status = store.load(nodes);
    if (status == radiosensors::registry::LoadStatus::Invalid ||
        status == radiosensors::registry::LoadStatus::StorageError || !boundStorage.begin()) {
        Serial.println("Node registry storage unavailable; reset or repair required");
        return false;
    }
    bounds.load();
    replayGuard.restart();
    forgetUnusedReplaySlots();
    secureRadio.restart();
    publishLockFreeView();
    initialized = true;
    Serial.printf(
        "Node registry ready: records=%u generation=%lu source=%s\n",
        static_cast<unsigned>(nodes.size()),
        static_cast<unsigned long>(store.generation()),
        status == radiosensors::registry::LoadStatus::Loaded ? "nvs" : "empty");
    return true;
}

bool ready() { return initialized && !reloadRequired.load(); }

void loop() {
    if (!initialized || !reloadRequired.load()) return;
    if (static_cast<int32_t>(millis() - nextReloadAt.load()) < 0) return;
    recovery::Guard guard(0);
    if (!guard || recovery::blocked() || xSemaphoreTake(mutex, 0) != pdTRUE) return;
    if (static_cast<int32_t>(millis() - nextReloadAt.load()) < 0) {
        xSemaphoreGive(mutex);
        return;
    }
    nextReloadAt = millis() + kReloadIntervalMs;
    const auto loaded = store.load(nodes);
    bool recovered = loaded == radiosensors::registry::LoadStatus::Loaded ||
        loaded == radiosensors::registry::LoadStatus::Empty;
    if (recovered) {
        bounds.load();
        recovered = bounds.writable();
    }
    if (recovered) {
        // An uncertain write may have committed; discard all pre-reload floors
        // and cached replies before accepting any frame under the loaded keys.
        replayGuard.restart();
        secureRadio.restart();
        recovered = forgetUnusedReplaySlots();
        if (recovered) reloadRequired.store(false);
    }
    publishLockFreeView();
    xSemaphoreGive(mutex);
    Serial.println(recovered ? "Radio storage reloaded; reception resumed"
                             : "Radio storage reload failed; reception remains disabled");
}

// Lock-free: a commit holds the mutex across an NVS write, and nothing should
// wait on that to read a counter. Use snapshot() when the generation and the
// records have to agree.
uint32_t generation() {
    return publishedGeneration.load(std::memory_order_acquire);
}

size_t recordCount() {
    if (mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
    const size_t value = nodes.size();
    xSemaphoreGive(mutex);
    return value;
}

bool isActiveNode(const uint8_t nodeId) {
    if (nodeId < radiosensors::registry::kFirstNodeId ||
        nodeId > radiosensors::registry::kLastNodeId) {
        return false;
    }
    const uint32_t word = activeNodeIds[nodeId / 32U].load(
        std::memory_order_acquire);
    return (word & (1UL << (nodeId % 32U))) != 0;
}

bool hasActiveNodes() {
    for (size_t index = 0; index < 4; ++index) {
        if (activeNodeIds[index].load(std::memory_order_acquire) != 0) return true;
    }
    return false;
}

bool activeProfileId(const uint8_t nodeId, uint16_t& profileId) {
    if (!ready() || mutex == nullptr) return false;
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const radiosensors::registry::NodeRecord* const record =
        nodes.findByNodeId(nodeId);
    const bool found = store.writable() && record != nullptr &&
        record->state == radiosensors::registry::NodeState::Active;
    if (found) profileId = record->profileId;
    xSemaphoreGive(mutex);
    return found;
}

bool activeIdentity(
    const uint8_t nodeId, uint8_t* const deviceUid, uint16_t& profileId) {
    if (!ready() || mutex == nullptr || deviceUid == nullptr) return false;
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const radiosensors::registry::NodeRecord* const record =
        nodes.findByNodeId(nodeId);
    const bool found = store.writable() && record != nullptr &&
        record->state == radiosensors::registry::NodeState::Active;
    if (found) {
        memcpy(deviceUid, record->deviceUid, sizeof(record->deviceUid));
        profileId = record->profileId;
    }
    xSemaphoreGive(mutex);
    return found;
}

bool radioPolicy(
    const uint8_t nodeId, uint8_t& maxPowerLevel, uint8_t& powerPolicy) {
    if (!ready() || mutex == nullptr) return false;
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const radiosensors::registry::NodeRecord* const record =
        nodes.findByNodeId(nodeId);
    const bool found = store.writable() && record != nullptr &&
        record->state == radiosensors::registry::NodeState::Active;
    if (found) {
        maxPowerLevel = record->maxPowerLevel;
        powerPolicy = record->powerPolicy;
    }
    xSemaphoreGive(mutex);
    return found;
}

bool snapshot(Snapshot& value) {
    if (!ready() || mutex == nullptr ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    value.generation = store.generation();
    if (!store.writable()) {
        xSemaphoreGive(mutex);
        return false;
    }
    value.count = nodes.size();
    if (value.count != 0) {
        memcpy(
            value.records, nodes.records(),
            value.count * sizeof(value.records[0]));
    }
    xSemaphoreGive(mutex);
    return true;
}

radiosensors::security::pairing::Status pairingRequestAndSave(
    const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
    radiosensors::security::Transport transport, const uint8_t* wire, size_t size,
    uint8_t networkId, radiosensors::security::Transport acceptTransport,
    uint8_t* output, size_t capacity) {
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || !mutex)
        return radiosensors::security::pairing::Status::StorageError;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto result = pairing.request(factoryMac, expectedUid, transport, wire, size,
                                       networkId, acceptTransport, output, capacity);
    publishLockFreeView();
    xSemaphoreGive(mutex);
    return result;
}

radiosensors::security::pairing::Status pairingConfirmAndSave(
    uint8_t nodeId, radiosensors::security::Transport transport,
    const uint8_t* wire, size_t size, uint8_t* output, size_t capacity) {
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || !mutex)
        return radiosensors::security::pairing::Status::StorageError;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto result = pairing.confirm(nodeId, transport, wire, size, output, capacity);
    publishLockFreeView();
    xSemaphoreGive(mutex);
    return result;
}

bool activeSecurity(uint8_t nodeId, radiosensors::security::Keys& keys, uint8_t& replaySlot) {
    if (!ready() || !mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool found = pairing.activeKeys(nodeId, keys, replaySlot);
    xSemaphoreGive(mutex);
    return found;
}

bool activeFrameIdentity(uint8_t nodeId, const uint8_t* salt, uint8_t* uid, uint16_t& profileId) {
    if (!ready() || !mutex || !salt || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    uint8_t currentSalt[radiosensors::security::kSaltSize];
    const auto* node = nodes.findByNodeId(nodeId);
    const bool found = node && pairing.activeSalt(nodeId, currentSalt) &&
        memcmp(salt, currentSalt, sizeof(currentSalt)) == 0;
    if (found) {
        profileId = node->profileId;
        if (uid) memcpy(uid, node->deviceUid, sizeof(node->deviceUid));
    }
    xSemaphoreGive(mutex);
    return found;
}

RadioDiagnostics radioDiagnostics() {
    const auto nvs = boundStorage.diagnostics();
    return {lastReceiveUs.load(), maxReceiveUs.load(), nvs.writes, nvs.failures,
            nvs.lastWriteUs, nvs.maxWriteUs, historyBytes.load()};
}

radiosensors::registry::ReceiveStatus receiveSecure(
    radiosensors::security::Transport transport, uint8_t* wire, size_t size,
    bool telemetrySpace, bool sessionSpace, radiosensors::registry::OpenedFrame& frame) {
    using radiosensors::registry::ReceiveStatus;
    if (gateway::recovery::blocked() || !ready() || !mutex) return ReceiveStatus::StorageError;
    // Both locks share a bounded wait so contention leaves time for the ACK.
    const TickType_t startedWaiting = xTaskGetTickCount();
    gateway::recovery::Guard guard(kRadioLockWait);
    if (!guard) return ReceiveStatus::Busy;
    if (gateway::recovery::blocked()) return ReceiveStatus::StorageError;
    if (xSemaphoreTake(mutex, remainingRadioLockWait(startedWaiting)) != pdTRUE) return ReceiveStatus::Busy;
    const int64_t started = esp_timer_get_time();
    const auto result = secureRadio.receive(transport, wire, size,
        static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL,
        telemetrySpace, sessionSpace, frame);
    if (!store.writable() || !bounds.writable()) publishLockFreeView();
    historyBytes.store(static_cast<uint32_t>(secureRadio.historyBytes()));
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
    lastReceiveUs.store(elapsed);
    if (elapsed > maxReceiveUs.load()) maxReceiveUs.store(elapsed);
    xSemaphoreGive(mutex);
    return result;
}

bool commandReply(uint8_t nodeId, const uint8_t* salt, uint32_t counter,
    uint8_t header, const uint8_t* payload, size_t size,
    uint8_t* output, size_t capacity, size_t& outputSize) {
    outputSize = 0;
    if (gateway::recovery::blocked() || !ready() || !mutex) return false;
    const TickType_t startedWaiting = xTaskGetTickCount();
    gateway::recovery::Guard guard(kRadioLockWait);
    if (!guard || gateway::recovery::blocked() ||
        xSemaphoreTake(mutex, remainingRadioLockWait(startedWaiting)) != pdTRUE) return false;
    const bool result = secureRadio.reply(nodeId, salt, counter, header, payload, size,
                                         output, capacity, outputSize);
    xSemaphoreGive(mutex);
    return result;
}

RegistryCommitStatus renameAndSave(
    const uint8_t nodeId, const char* const displayName, const size_t length,
    radiosensors::registry::RenameStatus& result) {
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || mutex == nullptr) return RegistryCommitStatus::NotInitialized;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint64_t lockStartedUs = commitClockUs();
    const std::unique_ptr<radiosensors::registry::NodeRegistry> candidate(
        new (std::nothrow) radiosensors::registry::NodeRegistry(nodes));
    if (!candidate) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::StorageError;
    }
    result = candidate->rename(nodeId, displayName, length);
    if (result != radiosensors::registry::RenameStatus::Renamed) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::NoChange;
    }
    const CommitTiming timing = commitCandidateLocked(*candidate, lockStartedUs);
    xSemaphoreGive(mutex);
    logCommitTiming("rename", timing);
    return timing.saved ? RegistryCommitStatus::Ok
                        : RegistryCommitStatus::StorageError;
}

RegistryCommitStatus setPowerPolicyAndSave(
    const uint8_t nodeId, const uint8_t policy,
    radiosensors::registry::PowerPolicyStatus& result) {
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || mutex == nullptr) return RegistryCommitStatus::NotInitialized;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint64_t lockStartedUs = commitClockUs();
    const std::unique_ptr<radiosensors::registry::NodeRegistry> candidate(
        new (std::nothrow) radiosensors::registry::NodeRegistry(nodes));
    if (!candidate) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::StorageError;
    }
    result = candidate->setPowerPolicy(nodeId, policy);
    if (result != radiosensors::registry::PowerPolicyStatus::Updated) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::NoChange;
    }
    const CommitTiming timing = commitCandidateLocked(*candidate, lockStartedUs);
    xSemaphoreGive(mutex);
    logCommitTiming("power_policy", timing);
    return timing.saved ? RegistryCommitStatus::Ok
                        : RegistryCommitStatus::StorageError;
}

RegistryCommitStatus updateInfoAndSave(
    const uint8_t nodeId, const uint8_t* const deviceUid,
    const radiosensors::protocol::NodeInfo& info,
    radiosensors::registry::InfoStatus& result) {
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || mutex == nullptr) return RegistryCommitStatus::NotInitialized;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint64_t lockStartedUs = commitClockUs();
    const std::unique_ptr<radiosensors::registry::NodeRegistry> candidate(
        new (std::nothrow) radiosensors::registry::NodeRegistry(nodes));
    if (!candidate) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::StorageError;
    }
    result = candidate->updateInfo(nodeId, deviceUid, info);
    if (result != radiosensors::registry::InfoStatus::Updated &&
        result != radiosensors::registry::InfoStatus::ProfileChanged) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::NoChange;
    }
    const CommitTiming timing = commitCandidateLocked(*candidate, lockStartedUs);
    xSemaphoreGive(mutex);
    logCommitTiming("node_info", timing);
    return timing.saved ? RegistryCommitStatus::Ok
                        : RegistryCommitStatus::StorageError;
}

RegistryCommitStatus removeAndSave(const uint8_t nodeId, bool& removed) {
    removed = false;
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || mutex == nullptr) return RegistryCommitStatus::NotInitialized;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint64_t lockStartedUs = commitClockUs();
    const std::unique_ptr<radiosensors::registry::NodeRegistry> candidate(
        new (std::nothrow) radiosensors::registry::NodeRegistry(nodes));
    if (!candidate) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::StorageError;
    }
    removed = candidate->remove(nodeId);
    if (!removed) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::NoChange;
    }
    const CommitTiming timing = commitCandidateLocked(*candidate, lockStartedUs);
    if (!timing.saved) removed = false;
    if (timing.saved) {
        forgetUnusedReplaySlots();
        publishLockFreeView();
    }
    xSemaphoreGive(mutex);
    logCommitTiming("remove", timing);
    return timing.saved ? RegistryCommitStatus::Ok
                        : RegistryCommitStatus::StorageError;
}

RegistryCommitStatus clearAndSave(size_t& removed) {
    removed = 0;
    gateway::recovery::Guard guard;
    if (!guard || gateway::recovery::blocked() || !ready() || mutex == nullptr) return RegistryCommitStatus::NotInitialized;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint64_t lockStartedUs = commitClockUs();
    const size_t previous = nodes.size();
    if (previous == 0) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::NoChange;
    }
    const std::unique_ptr<radiosensors::registry::NodeRegistry> empty(
        new (std::nothrow) radiosensors::registry::NodeRegistry());
    if (!empty) {
        xSemaphoreGive(mutex);
        return RegistryCommitStatus::StorageError;
    }
    const CommitTiming timing = commitCandidateLocked(*empty, lockStartedUs);
    if (timing.saved) {
        removed = previous;
        forgetUnusedReplaySlots();
        publishLockFreeView();
    }
    xSemaphoreGive(mutex);
    logCommitTiming("clear", timing);
    return timing.saved ? RegistryCommitStatus::Ok
                        : RegistryCommitStatus::StorageError;
}

}  // namespace gateway::registry_store
