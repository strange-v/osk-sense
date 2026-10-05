#include "ReplayBoundStorage.h"

namespace gateway {
namespace {
constexpr const char* kNamespace = "radio-bound";
constexpr const char* kKey = "bounds";
}

bool ReplayBoundStorage::begin() {
    initialized_ = preferences_.begin(kNamespace, false);
    return initialized_;
}

radiosensors::replay::ReadStatus ReplayBoundStorage::read(
    uint8_t* output, size_t capacity, size_t& size) {
    using radiosensors::replay::ReadStatus;
    size = 0;
    if (!initialized_ || !output) return ReadStatus::Error;
    if (!preferences_.isKey(kKey)) return ReadStatus::Missing;
    const size_t stored = preferences_.getBytesLength(kKey);
    if (stored != radiosensors::replay::kSnapshotSize) return ReadStatus::Invalid;
    if (stored > capacity) return ReadStatus::Error;
    if (preferences_.getBytes(kKey, output, stored) != stored) return ReadStatus::Error;
    size = stored;
    return ReadStatus::Ok;
}

bool ReplayBoundStorage::write(const uint8_t* data, size_t size) {
    return initialized_ && data && size == radiosensors::replay::kSnapshotSize &&
        preferences_.putBytes(kKey, data, size) == size;
}

}  // namespace gateway
