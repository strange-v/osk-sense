#pragma once

#include <GatewayStorage.h>

namespace radiosensors::command_book {

enum class QueueStatus : uint8_t {
    Queued,
    Busy,
    InvalidNode,
    InvalidArguments,
    CapacityReached,
};

enum class CompleteStatus : uint8_t {
    Completed,
    Retry,
    NotPending,
    InvalidResult,
};

const gateway_storage::CommandRecord *find(const gateway_storage::CommandBook &book,
                                           uint8_t nodeId);

// Replaces the node's result record, or any record left by another UID under
// the same node ID. When the book is full, the oldest result is evicted.
QueueStatus queue(gateway_storage::CommandBook &book,
                  const uint8_t *deviceUid,
                  uint8_t nodeId,
                  uint8_t type,
                  const uint8_t *arguments,
                  size_t argumentSize,
                  uint64_t nowUnixMs,
                  uint16_t &commandId);

// A storage failure on the node leaves the command pending for redelivery.
CompleteStatus complete(gateway_storage::CommandBook &book,
                        const uint8_t *deviceUid,
                        uint8_t nodeId,
                        uint16_t commandId,
                        protocol::CommandStatus status,
                        const uint8_t *data,
                        size_t dataSize,
                        uint64_t nowUnixMs);

// Only a pending command can be cancelled.
bool cancel(gateway_storage::CommandBook &book, uint8_t nodeId);
bool removeNode(gateway_storage::CommandBook &book, uint8_t nodeId);

// Keeps the command ID sequence.
void clear(gateway_storage::CommandBook &book);

} // namespace radiosensors::command_book
