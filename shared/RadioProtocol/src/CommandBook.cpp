#include "CommandBook.h"

#include <string.h>

#include "NodeRegistry.h"

namespace radiosensors::command_book {
namespace {

using gateway_storage::CommandBook;
using gateway_storage::CommandRecord;
using gateway_storage::CommandState;
using gateway_storage::kMaxCommandRecords;

int indexOf(const CommandBook& book, const uint8_t nodeId) {
    for (size_t index = 0; index < book.count; ++index) {
        if (book.records[index].nodeId == nodeId) return static_cast<int>(index);
    }
    return -1;
}

void erase(CommandBook& book, const size_t index) {
    for (size_t next = index + 1; next < book.count; ++next) {
        book.records[next - 1] = book.records[next];
    }
    --book.count;
    book.records[book.count] = CommandRecord{};
}

bool sameUid(const CommandRecord& record, const uint8_t* deviceUid) {
    return memcmp(record.deviceUid, deviceUid, protocol::kDeviceUidSize) == 0;
}

}  // namespace

const CommandRecord* find(const CommandBook& book, const uint8_t nodeId) {
    const int index = indexOf(book, nodeId);
    return index < 0 ? nullptr : &book.records[index];
}

QueueStatus queue(
    CommandBook& book, const uint8_t* const deviceUid, const uint8_t nodeId,
    const uint8_t type, const uint8_t* const arguments,
    const size_t argumentSize, const uint64_t nowUnixMs, uint16_t& commandId) {
    if (deviceUid == nullptr || nodeId < registry::kFirstNodeId ||
        nodeId > registry::kLastNodeId) {
        return QueueStatus::InvalidNode;
    }
    if (!protocol::validCommandArguments(type, arguments, argumentSize)) {
        return QueueStatus::InvalidArguments;
    }
    const int existing = indexOf(book, nodeId);
    if (existing >= 0) {
        const CommandRecord& record = book.records[existing];
        if (record.state == CommandState::Pending && sameUid(record, deviceUid)) {
            return QueueStatus::Busy;
        }
        erase(book, static_cast<size_t>(existing));
    }
    if (book.count == kMaxCommandRecords) {
        size_t oldest = 0;
        while (oldest < book.count &&
               book.records[oldest].state != CommandState::Completed) {
            ++oldest;
        }
        if (oldest == book.count) return QueueStatus::CapacityReached;
        erase(book, oldest);
    }

    CommandRecord& record = book.records[book.count++];
    record = CommandRecord{};
    memcpy(record.deviceUid, deviceUid, protocol::kDeviceUidSize);
    record.nodeId = nodeId;
    record.commandId = book.nextCommandId;
    record.type = type;
    record.argumentSize = static_cast<uint8_t>(argumentSize);
    if (argumentSize != 0) memcpy(record.arguments, arguments, argumentSize);
    record.state = CommandState::Pending;
    record.queuedAtUnixMs = nowUnixMs;
    commandId = record.commandId;
    book.nextCommandId = book.nextCommandId == UINT16_MAX
        ? 1
        : static_cast<uint16_t>(book.nextCommandId + 1U);
    return QueueStatus::Queued;
}

CompleteStatus complete(
    CommandBook& book, const uint8_t* const deviceUid, const uint8_t nodeId,
    const uint16_t commandId, const protocol::CommandStatus status,
    const uint8_t* const data, const size_t dataSize,
    const uint64_t nowUnixMs) {
    const int index = indexOf(book, nodeId);
    if (index < 0 || deviceUid == nullptr) return CompleteStatus::NotPending;
    CommandRecord& record = book.records[index];
    if (record.state != CommandState::Pending || record.commandId != commandId ||
        !sameUid(record, deviceUid)) {
        return CompleteStatus::NotPending;
    }
    if (!protocol::validCommandStatus(static_cast<uint8_t>(status))) {
        return CompleteStatus::InvalidResult;
    }
    if (status == protocol::CommandStatus::StorageFailure) {
        return CompleteStatus::Retry;
    }
    const size_t expected = protocol::commandResultDataSize(
        static_cast<protocol::CommandType>(record.type), status);
    if (dataSize != expected || (dataSize != 0 && data == nullptr)) {
        return CompleteStatus::InvalidResult;
    }
    record.state = CommandState::Completed;
    record.status = status;
    record.resultSize = static_cast<uint8_t>(dataSize);
    if (dataSize != 0) memcpy(record.result, data, dataSize);
    record.completedAtUnixMs = nowUnixMs;
    return CompleteStatus::Completed;
}

bool cancel(CommandBook& book, const uint8_t nodeId) {
    const int index = indexOf(book, nodeId);
    if (index < 0 || book.records[index].state != CommandState::Pending) return false;
    erase(book, static_cast<size_t>(index));
    return true;
}

bool removeNode(CommandBook& book, const uint8_t nodeId) {
    const int index = indexOf(book, nodeId);
    if (index < 0) return false;
    erase(book, static_cast<size_t>(index));
    return true;
}

void clear(CommandBook& book) {
    const uint16_t nextCommandId = book.nextCommandId;
    book = CommandBook{};
    book.nextCommandId = nextCommandId;
}

}  // namespace radiosensors::command_book
