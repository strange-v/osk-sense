#include "CommandService.h"

#include <CommandBook.h>
#include <NodeRegistry.h>
#include <RadioSecurityFrames.h>
#include <Preferences.h>
#include <esp_random.h>

#include <atomic>
#include <string.h>

#include "NodeRegistryStore.h"
#include "RadioService.h"
#include "RecoveryService.h"
#include "TelemetryStore.h"
#include "TimeService.h"

namespace gateway::commands {
namespace {

using namespace radiosensors::gateway_storage;
namespace command_book = radiosensors::command_book;
namespace protocol = radiosensors::protocol;
namespace frames = radiosensors::security::frames;

// As large as the commissioning task's: a read_info result commits the node
// registry here, and that NVS blob write overflows 6 KiB.
constexpr uint32_t kTaskStackSize = 8192;
// Above commissioning, so a node's session reply never waits behind a pairing
// commit; below the radio owner.
constexpr UBaseType_t kTaskPriority = 7;
constexpr BaseType_t kTaskCore = 1;
constexpr size_t kNodeIdSlots = radiosensors::registry::kLastNodeId + 1;

class NvsSlotStorage final : public SlotStorage {
public:
    NvsSlotStorage(const char* nameSpace, const char* slotA, const char* slotB)
        : nameSpace_(nameSpace), keys_{slotA, slotB} {}

    bool begin() { return preferences_.begin(nameSpace_, false); }

    bool exists(uint8_t slot) override {
        return slot < 2 && preferences_.getBytesLength(keys_[slot]) != 0;
    }

    bool read(uint8_t slot, uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (slot >= 2 || output == nullptr) return false;
        const size_t storedSize = preferences_.getBytesLength(keys_[slot]);
        if (storedSize == 0 || storedSize > capacity) return false;
        const size_t readSize = preferences_.getBytes(keys_[slot], output, storedSize);
        if (readSize != storedSize) return false;
        size = readSize;
        return true;
    }

    bool write(uint8_t slot, const uint8_t* data, size_t size) override {
        return slot < 2 && data != nullptr &&
            preferences_.putBytes(keys_[slot], data, size) == size;
    }

private:
    const char* nameSpace_;
    const char* keys_[2];
    Preferences preferences_;
};

// Results must follow the authenticated ready counter under the same keys.
// Delivery state stays in RAM; a restarted gateway delivers pending commands again.
struct Delivery {
    uint32_t readyCounter;
    uint8_t salt[radiosensors::security::kSaltSize];
    uint16_t commandId;
};

struct Counters {
    uint32_t sessions;
    uint32_t delivered;
    uint32_t noCommandReplies;
    uint32_t resultsRecorded;
    uint32_t duplicateResults;
    uint32_t rejectedFrames;
    uint32_t storageErrors;
};

NvsSlotStorage slots("node-cmd", "commands_a", "commands_b");
CommandStore store(slots);
CommandBook book{};
// Scratch copy for a commit, kept off the calling task's stack. Guarded by
// the mutex like `book`.
CommandBook candidate{};
Delivery deliveries[kNodeIdSlots]{};
SemaphoreHandle_t mutex = nullptr;
bool initialized = false;
std::atomic<uint32_t> pendingNodeIds[4]{};
std::atomic<uint32_t> publishedGeneration{0};
std::atomic<uint8_t> publishedRecords{0};
std::atomic<uint8_t> publishedPending{0};
Counters counters{};
portMUX_TYPE countersMux = portMUX_INITIALIZER_UNLOCKED;

void count(uint32_t Counters::*field) {
    portENTER_CRITICAL(&countersMux);
    ++(counters.*field);
    portEXIT_CRITICAL(&countersMux);
}

bool lock() {
    return initialized && mutex != nullptr &&
        xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

void unlock() { xSemaphoreGive(mutex); }

// The book state readers may see without the mutex. Call with the mutex held
// after every change of `book`.
void publishLockFreeView() {
    uint32_t words[4]{};
    uint8_t pending = 0;
    for (size_t index = 0; index < book.count; ++index) {
        const CommandRecord& record = book.records[index];
        if (record.state != CommandState::Pending) continue;
        words[record.nodeId / 32U] |= 1UL << (record.nodeId % 32U);
        ++pending;
    }
    for (size_t index = 0; index < 4; ++index) {
        pendingNodeIds[index].store(words[index], std::memory_order_release);
    }
    publishedGeneration.store(store.generation(), std::memory_order_release);
    publishedRecords.store(book.count, std::memory_order_release);
    publishedPending.store(pending, std::memory_order_release);
}

// Call with the mutex held.
bool commitCandidate() {
    if (!store.save(candidate)) return false;
    book = candidate;
    publishLockFreeView();
    return true;
}

uint16_t randomCommandId() {
    uint16_t value = 0;
    while (value == 0) value = static_cast<uint16_t>(esp_random());
    return value;
}

void sendNoCommand(const radio::ReceivedFrame& ready) {
    if (radio::sendCommandReply(ready, frames::kNoCommandHeader, nullptr, 0))
        count(&Counters::noCommandReplies);
}

void handleCommandReady(const radio::ReceivedFrame& received) {
    if (received.size != 0) { count(&Counters::rejectedFrames); return; }
    count(&Counters::sessions);
    const uint8_t nodeId = static_cast<uint8_t>(received.senderId);
    // The node is listening: answer an empty session without taking a lock.
    if (!hasPending(nodeId)) {
        sendNoCommand(received);
        return;
    }

    uint8_t deviceUid[protocol::kDeviceUidSize]{};
    uint16_t profileId = 0;
    const bool registered =
        registry_store::activeFrameIdentity(nodeId, received.salt, deviceUid, profileId);
    frames::Command command;
    bool deliver = false;
    if (lock()) {
        const CommandRecord* const record = command_book::find(book, nodeId);
        deliver = registered && record != nullptr &&
            record->state == CommandState::Pending &&
            memcmp(record->deviceUid, deviceUid, sizeof(deviceUid)) == 0;
        if (deliver) {
            command.commandId = record->commandId;
            command.type = record->type;
            command.argumentSize = record->argumentSize;
            memcpy(command.arguments, record->arguments, sizeof(command.arguments));
            deliveries[nodeId].readyCounter = received.counter;
            memcpy(deliveries[nodeId].salt, received.salt, sizeof(received.salt));
            deliveries[nodeId].commandId = record->commandId;
        }
        unlock();
    }
    uint8_t payload[frames::kMaxCommandPayloadSize]; size_t size = 0;
    if (!deliver || !frames::encodeCommand(command, payload, sizeof(payload), size)) {
        sendNoCommand(received);
        return;
    }
    if (!radio::sendCommandReply(received, frames::kCommandHeader, payload, size)) return;
    count(&Counters::delivered);
    Serial.printf(
        "Command delivered: node=%u id=%u type=%u\n",
        nodeId, command.commandId, command.type);
}

// Call with the mutex held, for a result matching the latest delivery. The
// registry is written before the command completes, so a failed write leaves
// the command pending and the node answers it again in a later session.
bool applyReadInfo(
    const uint8_t nodeId, const uint8_t* const deviceUid,
    const frames::CommandResult& result, bool& profileChanged) {
    const CommandRecord* const record = command_book::find(book, nodeId);
    if (record == nullptr || record->state != CommandState::Pending ||
        record->commandId != result.commandId ||
        record->type != static_cast<uint8_t>(protocol::CommandType::ReadInfo) ||
        result.status != protocol::CommandStatus::Applied) {
        return true;
    }
    protocol::NodeInfo info{};
    if (!protocol::decodeNodeInfo(result.data, result.dataSize, info)) {
        count(&Counters::rejectedFrames);
        return false;
    }
    radiosensors::registry::InfoStatus status{};
    const RegistryCommitStatus commit =
        registry_store::updateInfoAndSave(nodeId, deviceUid, info, status);
    if (commit == RegistryCommitStatus::StorageError ||
        commit == RegistryCommitStatus::NotInitialized) {
        count(&Counters::storageErrors);
        return false;
    }
    profileChanged = status == radiosensors::registry::InfoStatus::ProfileChanged;
    return true;
}

void handleCommandResult(const radio::ReceivedFrame& received) {
    frames::CommandResult result;
    if (!frames::decodeCommandResult(received.data, received.size, result)) {
        count(&Counters::rejectedFrames);
        return;
    }
    const uint8_t nodeId = static_cast<uint8_t>(received.senderId);
    uint8_t deviceUid[protocol::kDeviceUidSize]{};
    uint16_t profileId = 0;
    recovery::Guard guard;
    if (!guard || recovery::blocked() ||
        !registry_store::activeFrameIdentity(nodeId, received.salt, deviceUid, profileId) || !lock()) {
        count(&Counters::rejectedFrames);
        return;
    }
    const Delivery delivery = deliveries[nodeId];
    if (delivery.commandId != result.commandId ||
        received.counter <= delivery.readyCounter ||
        memcmp(delivery.salt, received.salt, sizeof(received.salt)) != 0) {
        unlock();
        count(&Counters::rejectedFrames);
        return;
    }
    bool profileChanged = false;
    if (!applyReadInfo(nodeId, deviceUid, result, profileChanged)) {
        unlock();
        return;
    }
    candidate = book;
    const command_book::CompleteStatus status = command_book::complete(
        candidate, deviceUid, nodeId, result.commandId, result.status,
        result.data, result.dataSize, time_service::unixTimeMs());
    bool recorded = false;
    switch (status) {
        case command_book::CompleteStatus::Completed:
            recorded = commitCandidate();
            count(recorded ? &Counters::resultsRecorded : &Counters::storageErrors);
            break;
        case command_book::CompleteStatus::Retry:
            break;
        case command_book::CompleteStatus::NotPending:
            count(&Counters::duplicateResults);
            break;
        case command_book::CompleteStatus::InvalidResult:
            count(&Counters::rejectedFrames);
            break;
    }
    unlock();
    // The cached frame was decoded under the previous profile.
    if (profileChanged) telemetry_store::erase(nodeId);
    Serial.printf(
        "Command result: node=%u id=%u status=%u recorded=%s\n",
        nodeId, result.commandId, static_cast<unsigned>(result.status),
        recorded ? "yes" : "no");
}

void task(void*) {
    for (;;) {
        radio::ReceivedFrame received{};
        if (!radio::receiveSessionFrame(received, portMAX_DELAY)) continue;
        if (received.kind == protocol::FrameKind::CommandReady) {
            handleCommandReady(received);
        } else if (received.kind == protocol::FrameKind::CommandResult) {
            handleCommandResult(received);
        }
    }
}

}  // namespace

bool begin(const bool radioReady) {
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        Serial.println("Command book mutex creation failed");
        return false;
    }
    if (!slots.begin()) {
        Serial.println("Command book NVS initialization failed");
        return false;
    }
    const LoadStatus status = store.load(book);
    // A new sequence starts at a random ID, so it is unlikely to meet an ID a
    // node recorded under a previous installation.
    if (status != LoadStatus::Loaded) book.nextCommandId = randomCommandId();
    publishLockFreeView();
    initialized = true;
    Serial.printf(
        "Command book ready: records=%u generation=%lu source=%s\n",
        static_cast<unsigned>(book.count),
        static_cast<unsigned long>(store.generation()),
        status == LoadStatus::Loaded
            ? "nvs"
            : (status == LoadStatus::Empty ? "empty" : "corrupt"));
    if (!radioReady) return true;
    if (xTaskCreatePinnedToCore(
            task, "commands", kTaskStackSize, nullptr, kTaskPriority,
            nullptr, kTaskCore) != pdPASS) {
        Serial.println("Command task creation failed");
        return false;
    }
    return true;
}

bool hasPending(const uint8_t nodeId) {
    if (nodeId < radiosensors::registry::kFirstNodeId ||
        nodeId > radiosensors::registry::kLastNodeId) {
        return false;
    }
    const uint32_t word =
        pendingNodeIds[nodeId / 32U].load(std::memory_order_acquire);
    return (word & (1UL << (nodeId % 32U))) != 0;
}

QueueResult queue(
    const uint8_t nodeId, const protocol::CommandType type,
    const uint8_t* const arguments, const size_t argumentSize, Entry& queued) {
    if (!initialized) return QueueResult::NotInitialized;
    uint8_t deviceUid[protocol::kDeviceUidSize]{};
    uint16_t profileId = 0;
    if (!registry_store::activeIdentity(nodeId, deviceUid, profileId)) {
        return QueueResult::NodeNotFound;
    }
    if (!protocol::profileSupportsCommand(profileId, type)) {
        return QueueResult::Unsupported;
    }
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !lock()) return QueueResult::NotInitialized;
    candidate = book;
    uint16_t commandId = 0;
    QueueResult result = QueueResult::StorageError;
    switch (command_book::queue(
        candidate, deviceUid, nodeId, static_cast<uint8_t>(type), arguments,
        argumentSize, time_service::unixTimeMs(), commandId)) {
        case command_book::QueueStatus::Queued:
            result = commitCandidate() ? QueueResult::Queued : QueueResult::StorageError;
            break;
        case command_book::QueueStatus::Busy:
            result = QueueResult::Busy;
            break;
        case command_book::QueueStatus::InvalidNode:
            result = QueueResult::NodeNotFound;
            break;
        case command_book::QueueStatus::InvalidArguments:
            result = QueueResult::InvalidArguments;
            break;
        case command_book::QueueStatus::CapacityReached:
            result = QueueResult::CapacityReached;
            break;
    }
    if (result == QueueResult::Queued) {
        queued.record = *command_book::find(book, nodeId);
        queued.delivered = false;
        deliveries[nodeId] = Delivery{};
    }
    unlock();
    return result;
}

CancelResult cancel(const uint8_t nodeId) {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !lock()) return CancelResult::NotInitialized;
    candidate = book;
    CancelResult result = CancelResult::NotFound;
    if (command_book::cancel(candidate, nodeId)) {
        result = commitCandidate() ? CancelResult::Cancelled : CancelResult::StorageError;
    }
    unlock();
    return result;
}

bool removeNode(const uint8_t nodeId) {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !lock()) return false;
    candidate = book;
    bool removed = command_book::removeNode(candidate, nodeId) && commitCandidate();
    if (nodeId < kNodeIdSlots) deliveries[nodeId] = Delivery{};
    unlock();
    return removed;
}

bool clear() {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !lock()) return false;
    candidate = book;
    command_book::clear(candidate);
    const bool cleared = book.count == 0 || commitCandidate();
    for (Delivery& delivery : deliveries) delivery = Delivery{};
    unlock();
    return cleared;
}

bool list(Listing& value) {
    if (!lock()) return false;
    value.count = book.count;
    for (size_t index = 0; index < book.count; ++index) {
        const CommandRecord& record = book.records[index];
        value.entries[index].record = record;
        value.entries[index].delivered =
            record.state == CommandState::Pending &&
            deliveries[record.nodeId].commandId == record.commandId;
    }
    unlock();
    return true;
}

Snapshot snapshot() {
    Snapshot value{};
    value.ready = initialized;
    value.generation = publishedGeneration.load(std::memory_order_acquire);
    value.records = publishedRecords.load(std::memory_order_acquire);
    value.pending = publishedPending.load(std::memory_order_acquire);
    portENTER_CRITICAL(&countersMux);
    value.sessions = counters.sessions;
    value.delivered = counters.delivered;
    value.noCommandReplies = counters.noCommandReplies;
    value.resultsRecorded = counters.resultsRecorded;
    value.duplicateResults = counters.duplicateResults;
    value.rejectedFrames = counters.rejectedFrames;
    value.storageErrors = counters.storageErrors;
    portEXIT_CRITICAL(&countersMux);
    return value;
}

}  // namespace gateway::commands
