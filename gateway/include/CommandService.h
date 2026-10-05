#pragma once

#include <Arduino.h>
#include <GatewayStorage.h>

namespace gateway::commands {

enum class QueueResult : uint8_t {
    Queued,
    NodeNotFound,
    Unsupported,
    InvalidArguments,
    Busy,
    CapacityReached,
    StorageError,
    NotInitialized,
};

enum class CancelResult : uint8_t {
    Cancelled,
    NotFound,
    StorageError,
    NotInitialized,
};

struct Entry {
    radiosensors::gateway_storage::CommandRecord record;
    // Sent to the node since this boot; its result has not arrived.
    bool delivered;
};

struct Listing {
    size_t count;
    Entry entries[radiosensors::gateway_storage::kMaxCommandRecords];
};

struct Snapshot {
    bool ready;
    uint32_t generation;
    uint8_t records;
    uint8_t pending;
    uint32_t sessions;
    uint32_t delivered;
    uint32_t noCommandReplies;
    uint32_t resultsRecorded;
    uint32_t duplicateResults;
    uint32_t rejectedFrames;
    uint32_t storageErrors;
};

// Loads the command book. Node sessions are answered only when the radio is.
bool begin(bool radioReady);
// Lock-free: the radio task asks it for every telemetry acknowledgement.
bool hasPending(uint8_t nodeId);
QueueResult queue(
    uint8_t nodeId, radiosensors::protocol::CommandType type,
    const uint8_t* arguments, size_t argumentSize, Entry& queued);
CancelResult cancel(uint8_t nodeId);
bool removeNode(uint8_t nodeId);
bool clear();
bool list(Listing& value);
Snapshot snapshot();
uint32_t stackFreeBytes();

}  // namespace gateway::commands
