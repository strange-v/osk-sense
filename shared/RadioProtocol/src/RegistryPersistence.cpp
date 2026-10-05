#include "RegistryPersistence.h"

#include "JoinRequest.h"
#include <string.h>
#ifndef __AVR__
#include <memory>
#include <new>
#endif

namespace radiosensors {
namespace registry {
namespace {

constexpr uint8_t kMagic[4] = {'R', 'S', 'N', 'R'};

uint32_t crc32(const uint8_t* data, const size_t size) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(
                -static_cast<int32_t>(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}


}  // namespace

SnapshotStatus encodeRegistrySnapshot(
    const NodeRegistry& registry,
    const uint32_t generation,
    uint8_t* const output,
    const size_t outputSize,
    size_t& encodedSize) {
    const size_t requiredSize = kRegistryHeaderSize +
        registry.size() * kStoredNodeRecordSize + kRegistryCrcSize;
    encodedSize = 0;
    if (output == nullptr || outputSize < requiredSize) {
        return SnapshotStatus::OutputTooSmall;
    }

    output[0] = kMagic[0];
    output[1] = kMagic[1];
    output[2] = kMagic[2];
    output[3] = kMagic[3];
    protocol::writeUint16Le(output + 4, kRegistryStorageVersion);
    protocol::writeUint32Le(output + 6, generation);
    output[10] = static_cast<uint8_t>(registry.size());
    output[11] = 0;

    size_t offset = kRegistryHeaderSize;
    const NodeRecord* const records = registry.records();
    for (size_t index = 0; index < registry.size(); ++index) {
        const NodeRecord& record = records[index];
        for (size_t uidIndex = 0; uidIndex < protocol::kDeviceUidSize; ++uidIndex) {
            output[offset + uidIndex] = record.deviceUid[uidIndex];
        }
        output[offset + 10] = record.nodeId;
        protocol::writeUint16Le(output + offset + 11, record.profileId);
        output[offset + 13] = record.firmware.major;
        output[offset + 14] = record.firmware.minor;
        output[offset + 15] = record.firmware.patch;
        output[offset + 16] = static_cast<uint8_t>(record.state);
        protocol::writeUint32Le(output + offset + 17, record.requestNonce);
        output[offset + 21] = record.displayNameLength;
        for (size_t nameIndex = 0; nameIndex < kNodeDisplayNameSize; ++nameIndex) {
            output[offset + 22 + nameIndex] = record.displayName[nameIndex];
        }
        output[offset + 22 + kNodeDisplayNameSize] = record.maxPowerLevel;
        output[offset + 23 + kNodeDisplayNameSize] = record.powerPolicy;
        output[offset + 24 + kNodeDisplayNameSize] = record.replaySlot;
        memcpy(output + offset + 25 + kNodeDisplayNameSize, record.pairing, sizeof(record.pairing));
        offset += kStoredNodeRecordSize;
    }

    protocol::writeUint32Le(output + offset, crc32(output, offset));
    encodedSize = requiredSize;
    return SnapshotStatus::Ok;
}

static SnapshotStatus decodeRegistrySnapshotUsingRecords(
    const uint8_t* const data,
    const size_t size,
    NodeRegistry& registry,
    uint32_t& generation,
    NodeRecord* const records) {
    if (data == nullptr || size < kRegistryHeaderSize + kRegistryCrcSize) {
        return SnapshotStatus::InvalidSize;
    }
    if (data[0] != kMagic[0] || data[1] != kMagic[1] ||
        data[2] != kMagic[2] || data[3] != kMagic[3]) {
        return SnapshotStatus::InvalidMagic;
    }
    if (protocol::readUint16Le(data + 4) != kRegistryStorageVersion) {
        return SnapshotStatus::UnsupportedVersion;
    }

    const size_t count = data[10];
    if (count > kMaxNodes) {
        return SnapshotStatus::InvalidRecordCount;
    }
    const size_t expectedSize = kRegistryHeaderSize +
        count * kStoredNodeRecordSize + kRegistryCrcSize;
    if (size != expectedSize) {
        return SnapshotStatus::InvalidSize;
    }
    const uint32_t storedCrc = protocol::readUint32Le(data + size - kRegistryCrcSize);
    if (storedCrc != crc32(data, size - kRegistryCrcSize)) {
        return SnapshotStatus::CrcMismatch;
    }

    for (size_t index = 0; index < kMaxNodes; ++index) {
        records[index] = NodeRecord{};
    }
    size_t offset = kRegistryHeaderSize;
    for (size_t index = 0; index < count; ++index) {
        NodeRecord& record = records[index];
        for (size_t uidIndex = 0; uidIndex < protocol::kDeviceUidSize; ++uidIndex) {
            record.deviceUid[uidIndex] = data[offset + uidIndex];
        }
        record.nodeId = data[offset + 10];
        record.profileId = protocol::readUint16Le(data + offset + 11);
        record.firmware = protocol::FirmwareVersion{
            data[offset + 13], data[offset + 14], data[offset + 15]};
        record.state = static_cast<NodeState>(data[offset + 16]);
        record.requestNonce = protocol::readUint32Le(data + offset + 17);
        record.displayNameLength = data[offset + 21];
        if (record.displayNameLength > kNodeDisplayNameSize)
            return SnapshotStatus::InvalidRegistry;
        for (size_t nameIndex = 0; nameIndex < kNodeDisplayNameSize; ++nameIndex) {
            record.displayName[nameIndex] = data[offset + 22 + nameIndex];
        }
        record.maxPowerLevel = data[offset + 22 + kNodeDisplayNameSize];
        record.powerPolicy = data[offset + 23 + kNodeDisplayNameSize];
        record.replaySlot = data[offset + 24 + kNodeDisplayNameSize];
        memcpy(record.pairing, data + offset + 25 + kNodeDisplayNameSize, sizeof(record.pairing));
        offset += kStoredNodeRecordSize;
    }

    if (!registry.restore(records, count)) {
        return SnapshotStatus::InvalidRegistry;
    }
    generation = protocol::readUint32Le(data + 6);
    return SnapshotStatus::Ok;
}

SnapshotStatus decodeRegistrySnapshot(
    const uint8_t* const data,
    const size_t size,
    NodeRegistry& registry,
    uint32_t& generation) {
#ifndef __AVR__
    std::unique_ptr<NodeRecord[]> records(new (std::nothrow) NodeRecord[kMaxNodes]{});
    if (!records) return SnapshotStatus::InvalidRegistry;
    return decodeRegistrySnapshotUsingRecords(
        data, size, registry, generation, records.get());
#else
    NodeRecord records[kMaxNodes]{};
    return decodeRegistrySnapshotUsingRecords(data, size, registry, generation, records);
#endif
}

#ifndef __AVR__
LoadStatus AtomicRegistryStore::load(NodeRegistry& registry) {
    writable_ = false;
    generation_ = 0;
    registry.restore(nullptr, 0);
    size_t size = 0;
    const auto status = storage_.read(buffer_, sizeof(buffer_), size);
    if (status == replay::ReadStatus::Missing) {
        writable_ = true;
        return LoadStatus::Empty;
    }
    if (status == replay::ReadStatus::Error) return LoadStatus::StorageError;
    if (status != replay::ReadStatus::Ok ||
        decodeRegistrySnapshot(buffer_, size, registry, generation_) != SnapshotStatus::Ok)
        return LoadStatus::Invalid;
    writable_ = true;
    return LoadStatus::Loaded;
}

bool AtomicRegistryStore::save(const NodeRegistry& registry) {
    if (!writable_) return false;
    size_t size = 0;
    const uint32_t next = generation_ + 1;
    if (encodeRegistrySnapshot(registry, next, buffer_, sizeof(buffer_), size) != SnapshotStatus::Ok)
        return false;
    std::unique_ptr<uint8_t[]> readback(new (std::nothrow) uint8_t[size]);
    if (!readback) return false;
    size_t readSize = 0;
    if (!storage_.write(buffer_, size) ||
        storage_.read(readback.get(), size, readSize) != replay::ReadStatus::Ok ||
        readSize != size || memcmp(buffer_, readback.get(), size) != 0) {
        writable_ = false;
        return false;
    }
    generation_ = next;
    return true;
}
#endif


}  // namespace registry
}  // namespace radiosensors
