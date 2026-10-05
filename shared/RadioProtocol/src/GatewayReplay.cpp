#include "GatewayReplay.h"

#include <string.h>
#include "JoinRequest.h"

namespace radiosensors {
namespace replay {
namespace {
constexpr uint8_t kMagic[4] = {'R', 'S', 'R', 'B'};
constexpr uint16_t kVersion = 3;
constexpr size_t kPresenceOffset = 12;
constexpr size_t kBoundFlagsOffset = 20;
constexpr size_t kBoundsOffset = 28;
constexpr size_t kCrcOffset = kSnapshotSize - 4;
static_assert(kBoundsOffset + kNodeSlots * 4 == kCrcOffset, "bound layout");

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return ~crc;
}
bool equal(const Snapshot& left, const Snapshot& right) {
    for (size_t i = 0; i < kNodeSlots; ++i)
        if (left.records[i].state != right.records[i].state ||
            left.records[i].upperBound != right.records[i].upperBound) return false;
    return true;
}
}

bool encode(const Snapshot& snapshot, uint32_t generation, uint8_t* output,
            size_t size) {
    if (!output || size != kSnapshotSize) return false;
    memset(output, 0, size);
    memcpy(output, kMagic, sizeof(kMagic));
    protocol::writeUint16Le(output + 4, kVersion);
    protocol::writeUint32Le(output + 6, generation);
    protocol::writeUint16Le(output + 10, kSnapshotSize);
    for (size_t i = 0; i < kNodeSlots; ++i) {
        const Record& record = snapshot.records[i];
        if (record.state != RecordState::Absent && record.state != RecordState::Paired &&
            record.state != RecordState::Bound) return false;
        if (record.state != RecordState::Bound && record.upperBound != 0) return false;
        if (record.state != RecordState::Absent)
            output[kPresenceOffset + i / 8] |= 1U << (i % 8);
        if (record.state == RecordState::Bound)
            output[kBoundFlagsOffset + i / 8] |= 1U << (i % 8);
        protocol::writeUint32Le(output + kBoundsOffset + i * 4, record.upperBound);
    }
    protocol::writeUint32Le(output + kCrcOffset, crc32(output, kCrcOffset));
    return true;
}

bool decode(const uint8_t* data, size_t size, Snapshot& snapshot, uint32_t& generation) {
    if (!data || size != kSnapshotSize || memcmp(data, kMagic, sizeof(kMagic)) != 0 ||
        protocol::readUint16Le(data + 4) != kVersion ||
        protocol::readUint16Le(data + 10) != kSnapshotSize ||
        protocol::readUint32Le(data + kCrcOffset) != crc32(data, kCrcOffset)) return false;
    Snapshot candidate;
    for (size_t i = 0; i < kNodeSlots; ++i) {
        const uint8_t mask = 1U << (i % 8);
        const bool present = data[kPresenceOffset + i / 8] & mask;
        const bool bound = data[kBoundFlagsOffset + i / 8] & mask;
        if (bound && !present) return false;
        Record& record = candidate.records[i];
        record.state = bound ? RecordState::Bound : present ? RecordState::Paired : RecordState::Absent;
        record.upperBound = protocol::readUint32Le(data + kBoundsOffset + i * 4);
        if (!bound && record.upperBound != 0) return false;
    }
    snapshot = candidate;
    generation = protocol::readUint32Le(data + 6);
    return true;
}

LoadStatus Store::load() {
    snapshot_ = Snapshot{};
    generation_ = 0;
    writable_ = true;
    hasSnapshot_ = false;
    uint8_t bytes[kSnapshotSize];
    size_t size = 0;
    const ReadStatus status = storage_.read(bytes, sizeof(bytes), size);
    if (status == ReadStatus::Missing) return LoadStatus::Empty;
    if (status == ReadStatus::Error) {
        writable_ = false;
        return LoadStatus::Unactivated;
    }
    if (status != ReadStatus::Ok) return LoadStatus::Unactivated;
    if (!decode(bytes, size, snapshot_, generation_)) return LoadStatus::Unactivated;
    hasSnapshot_ = true;
    return LoadStatus::Loaded;
}

bool Store::save(const Snapshot& snapshot) {
    if (!writable_) return false;
    if (hasSnapshot_ && equal(snapshot_, snapshot)) return true;
    uint8_t bytes[kSnapshotSize], readback[kSnapshotSize];
    const uint32_t next = generation_ + 1;
    if (!encode(snapshot, next, bytes, sizeof(bytes))) return false;
    size_t size = 0;
    if (!storage_.write(bytes, sizeof(bytes)) ||
        storage_.read(readback, sizeof(readback), size) != ReadStatus::Ok ||
        size != sizeof(bytes) || memcmp(bytes, readback, sizeof(bytes)) != 0) {
        // A failed read-back can hide a committed write. Reload before any retry.
        writable_ = false;
        return false;
    }
    snapshot_ = snapshot;
    hasSnapshot_ = true;
    generation_ = next;
    return true;
}

void AcceptanceHistory::begin(uint64_t now) {
    startedAt_ = lastAt_ = now;
    head_ = count_ = 0;
    initialized_ = true;
}

void AcceptanceHistory::expire(uint64_t now) {
    constexpr uint32_t day = 86400;
    if (!initialized_ || now < lastAt_) {
        begin(now);
        return;
    }
    if (now - lastAt_ >= day) {
        head_ = count_ = 0;
    } else {
        while (count_ && static_cast<uint32_t>(now) - times_[head_] >= day) {
            head_ = (head_ + 1) % kMaxWindow;
            --count_;
        }
    }
    lastAt_ = now;
}

uint16_t AcceptanceHistory::window(uint64_t now) {
    expire(now);
    if (now - startedAt_ < 86400) return kMaxWindow;
    return count_ ? count_ : 1;
}

void AcceptanceHistory::accepted(uint64_t now) {
    expire(now);
    if (count_ == kMaxWindow) {
        head_ = (head_ + 1) % kMaxWindow;
        --count_;
    }
    times_[(head_ + count_) % kMaxWindow] = static_cast<uint32_t>(now);
    ++count_;
}

void Guard::restart() {
    for (size_t i = 0; i < kNodeSlots; ++i) {
        Runtime& runtime = runtime_[i];
        runtime = Runtime{};
        const Record& record = store_.snapshot().records[i];
        if (record.state == RecordState::Bound) {
            runtime.exhausted = record.upperBound == UINT32_MAX;
            if (!runtime.exhausted) runtime.floor = record.upperBound + 1;
        }
    }
}

bool Guard::initializeFreshKeys(size_t slot) {
    if (slot >= kNodeSlots || store_.snapshot().records[slot].state != RecordState::Absent)
        return false;
    Snapshot candidate = store_.snapshot();
    candidate.records[slot] = Record{RecordState::Paired, 0};
    if (!store_.save(candidate)) return false;
    runtime_[slot] = Runtime{};
    return true;
}

bool Guard::forget(size_t slot) {
    if (slot >= kNodeSlots) return false;
    Snapshot candidate = store_.snapshot();
    candidate.records[slot] = Record{};
    if (!store_.save(candidate)) return false;
    runtime_[slot] = Runtime{};
    return true;
}

Decision Guard::inspect(size_t slot, uint32_t counter, uint16_t window,
                        const uint8_t* activationChallenge) {
    if (slot >= kNodeSlots) return Decision{Action::InvalidSlot};
    if (!store_.writable()) return Decision{Action::StorageError};
    Runtime& runtime = runtime_[slot];
    const Record& record = store_.snapshot().records[slot];
    if (record.state == RecordState::Absent) {
        uint8_t difference = 0;
        if (runtime.hasChallenge && activationChallenge)
            for (size_t i = 0; i < kChallengeSize; ++i)
                difference |= runtime.challenge[i] ^ activationChallenge[i];
        if (!runtime.hasChallenge || !activationChallenge || difference != 0) {
            if (!runtime.hasChallenge) {
                if (!random_.fill(runtime.challenge, kChallengeSize))
                    return Decision{Action::StorageError};
                runtime.hasChallenge = true;
            }
            Decision decision{Action::Challenge};
            memcpy(decision.challenge, runtime.challenge, kChallengeSize);
            return decision;
        }
    } else {
        if (runtime.exhausted) return Decision{Action::Exhausted};
        if (runtime.hasLast && counter == runtime.last) return Decision{Action::Duplicate};
        if (counter < runtime.floor) return Decision{Action::CounterFloor, runtime.floor};
    }
    if (window < 1) window = 1;
    if (window > kMaxWindow) window = kMaxWindow;
    if (record.state != RecordState::Bound || counter > record.upperBound) {
        const uint32_t increment = window - 1;
        const uint32_t bound = counter > UINT32_MAX - increment ? UINT32_MAX : counter + increment;
        Snapshot candidate = store_.snapshot();
        candidate.records[slot] = Record{RecordState::Bound, bound};
        if (!store_.save(candidate)) return Decision{Action::StorageError};
    }
    runtime.last = counter;
    runtime.hasLast = true;
    runtime.exhausted = counter == UINT32_MAX;
    if (!runtime.exhausted) runtime.floor = counter + 1;
    runtime.hasChallenge = false;
    return Decision{Action::Accept};
}

}  // namespace replay
}  // namespace radiosensors
