#pragma once

#include <stddef.h>
#include <stdint.h>

namespace radiosensors {
namespace replay {

constexpr size_t kNodeSlots = 64;
constexpr size_t kSnapshotSize = 288;
constexpr uint16_t kMaxWindow = 256;
constexpr size_t kChallengeSize = 8;

enum class RecordState : uint8_t { Absent, Paired, Bound };
struct Record {
    RecordState state = RecordState::Absent;
    uint32_t upperBound = 0;
};
struct Snapshot {
    Record records[kNodeSlots]{};
};

bool encode(const Snapshot& snapshot, uint32_t generation, uint8_t* output,
            size_t size);
bool decode(const uint8_t* data, size_t size, Snapshot& snapshot,
            uint32_t& generation);

enum class ReadStatus : uint8_t { Missing, Ok, Invalid, Error };
class BlobStorage {
public:
    virtual ~BlobStorage() {}
    virtual ReadStatus read(uint8_t* output, size_t capacity,
                            size_t& size) = 0;
    // The backend commits the replacement before returning success.
    virtual bool write(const uint8_t* data, size_t size) = 0;
};
enum class LoadStatus : uint8_t { Loaded, Empty, Unactivated };

class Store {
public:
    explicit Store(BlobStorage& storage) : storage_(storage) {}
    // Missing or invalid bounds require activation for nodes with existing keys.
    LoadStatus load();
    bool save(const Snapshot& snapshot);
    const Snapshot& snapshot() const { return snapshot_; }
    uint32_t generation() const { return generation_; }
    bool writable() const { return writable_; }
private:
    BlobStorage& storage_;
    Snapshot snapshot_{};
    uint32_t generation_ = 0;
    bool writable_ = false;
    bool hasSnapshot_ = false;
};

class RandomSource {
public:
    virtual ~RandomSource() {}
    virtual bool fill(uint8_t* output, size_t size) = 0;
};

// Counts only accepted frames in the trailing 24 hours, capped at 256.
// Use monotonic uptime seconds, not the adjustable wall clock.
class AcceptanceHistory {
public:
    void begin(uint64_t now);
    uint16_t window(uint64_t now);
    void accepted(uint64_t now);
private:
    void expire(uint64_t now);
    uint32_t times_[kMaxWindow]{};
    uint64_t startedAt_ = 0;
    uint64_t lastAt_ = 0;
    uint16_t head_ = 0;
    uint16_t count_ = 0;
    bool initialized_ = false;
};

enum class Action : uint8_t {
    Accept, Duplicate, CounterFloor, Challenge, Exhausted, StorageError, InvalidSlot
};
struct Decision {
    Action action = Action::StorageError;
    uint32_t floor = 0;
    uint8_t challenge[kChallengeSize]{};
};

class Guard {
public:
    Guard(Store& store, RandomSource& random) : store_(store), random_(random) {}
    // Call after Store::load(), including every gateway restart or backup restore.
    void restart();
    // Only for newly derived keys. Repeated Join confirm must not call this.
    // A present record cannot be reset; key replacement first forgets its slot.
    bool initializeFreshKeys(size_t slot);
    bool forget(size_t slot);
    // Call only after authenticating the complete frame and selecting its key slot.
    // An activation supplies the clear challenge covered by that frame's tag.
    // Publish only on Accept; Duplicate may only re-ACK or send a cached reply.
    Decision inspect(size_t slot, uint32_t counter, uint16_t window = kMaxWindow,
                     const uint8_t* activationChallenge = nullptr);
private:
    struct Runtime {
        uint32_t floor = 0;
        uint32_t last = 0;
        bool hasLast = false;
        bool exhausted = false;
        bool hasChallenge = false;
        uint8_t challenge[kChallengeSize]{};
    };
    Store& store_;
    RandomSource& random_;
    Runtime runtime_[kNodeSlots]{};
};

}  // namespace replay
}  // namespace radiosensors
