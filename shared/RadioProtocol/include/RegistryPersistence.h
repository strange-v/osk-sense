#pragma once

#include <stddef.h>
#include <stdint.h>

#include "NodeRegistry.h"

namespace radiosensors {
namespace registry {

constexpr uint16_t kRegistryStorageVersion = 4;
constexpr size_t kRegistryHeaderSize = 12;
constexpr size_t kStoredNodeRecordSize =
    21 + 1 + kNodeDisplayNameSize + 2 + 1 + security::pairing::kSnapshotSize;
constexpr size_t kRegistryCrcSize = 4;
constexpr size_t kMaxRegistrySnapshotSize =
    kRegistryHeaderSize + kMaxNodes * kStoredNodeRecordSize + kRegistryCrcSize;

enum class SnapshotStatus : uint8_t {
    Ok,
    OutputTooSmall,
    InvalidSize,
    InvalidMagic,
    UnsupportedVersion,
    InvalidRecordCount,
    CrcMismatch,
    InvalidRegistry,
};

SnapshotStatus encodeRegistrySnapshot(
    const NodeRegistry& registry,
    uint32_t generation,
    uint8_t* output,
    size_t outputSize,
    size_t& encodedSize);

SnapshotStatus decodeRegistrySnapshot(
    const uint8_t* data,
    size_t size,
    NodeRegistry& registry,
    uint32_t& generation);


enum class LoadStatus : uint8_t {
    Loaded,
    Empty,
    Invalid,
    StorageError,
};

// NVS journals one committed registry blob. Never recover a retired transaction
// from an older application-level copy after corruption.
class AtomicRegistryStore {
public:
    explicit AtomicRegistryStore(replay::BlobStorage& storage) : storage_(storage) {}
    LoadStatus load(NodeRegistry& registry);
    bool save(const NodeRegistry& registry);
    uint32_t generation() const { return generation_; }
    bool writable() const { return writable_; }
private:
    replay::BlobStorage& storage_;
    uint8_t buffer_[kMaxRegistrySnapshotSize]{};
    uint32_t generation_ = 0;
    bool writable_ = false;
};


}  // namespace registry
}  // namespace radiosensors
