#pragma once

#include "NodeStorage.h"

namespace radiosensors {
namespace node {
namespace security_storage {

constexpr size_t kConfigSize = 26;
constexpr size_t kReserveSize = 6;
constexpr size_t kConfigA = 0x00;
constexpr size_t kReserveA = 0x1A;
constexpr size_t kConfigB = 0x20;
constexpr size_t kReserveB = 0x3A;
constexpr uint32_t kReservationSize = 1024;
constexpr uint32_t kMaxFloorAdvance = 256;
static_assert(kConfigA + kConfigSize == kReserveA &&
              kReserveA + kReserveSize == kConfigB &&
              kConfigB + kConfigSize == kReserveB &&
              kReserveB + kReserveSize == storage::kProfileAreaStart,
              "security records must not overlap the profile area");

struct NetworkConfig {
    uint8_t generation = 0;
    storage::ProvisioningState state = storage::ProvisioningState::Provisional;
    uint8_t nodeId = 0;
    uint8_t gatewayId = 0;
    uint8_t networkId = 0;
    uint8_t salt[8]{};
    uint64_t requestNonce = 0;
};

inline void write64(uint8_t* output, uint64_t value) {
    storage::write32(output, static_cast<uint32_t>(value));
    storage::write32(output + 4, static_cast<uint32_t>(value >> 32));
}
inline uint64_t read64(const uint8_t* input) {
    return storage::read32(input) |
        (static_cast<uint64_t>(storage::read32(input + 4)) << 32);
}
inline bool encodeConfig(const NetworkConfig& value, uint8_t* output, size_t capacity) {
    const uint8_t state = static_cast<uint8_t>(value.state);
    if (output == nullptr || capacity < kConfigSize ||
        value.nodeId == 0 || value.nodeId == 255 ||
        value.gatewayId == 0 || value.gatewayId == 255 ||
        value.nodeId == value.gatewayId ||
        (state != 1 && state != 2)) return false;
    output[0] = 'R';
    output[1] = 'N';
    output[2] = 3;
    output[3] = value.generation;
    output[4] = state;
    output[5] = value.nodeId;
    output[6] = value.gatewayId;
    output[7] = value.networkId;
    memcpy(output + 8, value.salt, sizeof(value.salt));
    write64(output + 16, value.requestNonce);
    storage::write16(output + 24, storage::crc16Ccitt(output, 24));
    return true;
}
inline bool decodeConfig(const uint8_t* input, size_t size, NetworkConfig& value) {
    if (input == nullptr || size != kConfigSize ||
        input[0] != 'R' || input[1] != 'N' || input[2] != 3 ||
        (input[4] != 1 && input[4] != 2) ||
        input[5] == 0 || input[5] == 255 || input[6] == 0 || input[6] == 255 ||
        input[5] == input[6] ||
        storage::read16(input + 24) != storage::crc16Ccitt(input, 24)) return false;
    value.generation = input[3];
    value.state = static_cast<storage::ProvisioningState>(input[4]);
    value.nodeId = input[5];
    value.gatewayId = input[6];
    value.networkId = input[7];
    memcpy(value.salt, input + 8, sizeof(value.salt));
    value.requestNonce = read64(input + 16);
    return true;
}

template <typename Storage>
class NetworkConfigStore {
public:
    constexpr explicit NetworkConfigStore(Storage& storage) : storage_(storage) {}

    bool load(NetworkConfig& value) {
        uint8_t bytes[kConfigSize];
        NetworkConfig a, b;
        storage::readBlock(storage_, kConfigA, bytes, sizeof(bytes));
        const bool validA = decodeConfig(bytes, sizeof(bytes), a);
        storage::readBlock(storage_, kConfigB, bytes, sizeof(bytes));
        const bool validB = decodeConfig(bytes, sizeof(bytes), b);
        initialized_ = true;
        hasValue_ = validA || validB;
        if (!hasValue_) { slot_ = 1; generation_ = 0; return false; }
        if (validA && (!validB || storage::generationIsNewer(a.generation, b.generation))) {
            value = a;
            slot_ = 0;
        } else {
            value = b;
            slot_ = 1;
        }
        generation_ = value.generation;
        return true;
    }

    bool save(NetworkConfig& value) {
        if (!initialized_) { NetworkConfig ignored; load(ignored); }
        NetworkConfig candidate = value;
        candidate.generation = hasValue_ ? static_cast<uint8_t>(generation_ + 1) : 0;
        uint8_t bytes[kConfigSize];
        if (!encodeConfig(candidate, bytes, sizeof(bytes))) return false;
        const uint8_t nextSlot = slot_ ^ 1;
        const size_t address = nextSlot == 0 ? kConfigA : kConfigB;
        storage_.update(address, 0);
        storage_.update(address + 1, 0);
        for (size_t i = 2; i < sizeof(bytes); ++i) storage_.update(address + i, bytes[i]);
        storage_.update(address + 1, bytes[1]);
        storage_.update(address, bytes[0]);
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            if (storage_.read(address + i) != bytes[i]) return false;
        }
        value = candidate;
        slot_ = nextSlot;
        generation_ = value.generation;
        hasValue_ = true;
        return true;
    }

    bool factoryReset() {
        storage_.update(kConfigA, 0);
        storage_.update(kConfigA + 1, 0);
        storage_.update(kConfigB, 0);
        storage_.update(kConfigB + 1, 0);
        if (storage_.read(kConfigA) != 0 || storage_.read(kConfigA + 1) != 0 ||
            storage_.read(kConfigB) != 0 || storage_.read(kConfigB + 1) != 0) return false;
        initialized_ = true;
        hasValue_ = false;
        slot_ = 1;
        generation_ = 0;
        return true;
    }

private:
    Storage& storage_;
    bool initialized_ = false;
    bool hasValue_ = false;
    uint8_t slot_ = 1;
    uint8_t generation_ = 0;
};

enum class CounterLoadStatus : uint8_t { Fresh, Loaded, KeysMustBeDiscarded };

inline void encodeReserve(uint32_t limit, uint8_t output[kReserveSize]) {
    storage::write32(output, limit);
    storage::write16(output + 4, storage::crc16Ccitt(output, 4));
}
inline bool decodeReserve(const uint8_t input[kReserveSize], uint32_t& limit) {
    if (storage::read16(input + 4) != storage::crc16Ccitt(input, 4)) return false;
    limit = storage::read32(input);
    return true;
}

template <typename Storage>
class FrameCounterStore {
public:
    constexpr explicit FrameCounterStore(Storage& storage) : storage_(storage) {}

    // With configuration present, failure prohibits all transmission until
    // its keys are discarded. Re-pairing then establishes a fresh salt.
    CounterLoadStatus load(bool hasNetworkConfig) {
        uint8_t bytes[kReserveSize];
        uint32_t a = 0, b = 0;
        storage::readBlock(storage_, kReserveA, bytes, sizeof(bytes));
        const bool validA = decodeReserve(bytes, a);
        storage::readBlock(storage_, kReserveB, bytes, sizeof(bytes));
        const bool validB = decodeReserve(bytes, b);
        hasIssued_ = false;
        if (!validA && !validB) {
            next_ = limit_ = 0;
            slot_ = 1;
            healthy_ = !hasNetworkConfig;
            return healthy_ ? CounterLoadStatus::Fresh : CounterLoadStatus::KeysMustBeDiscarded;
        }
        slot_ = validA && (!validB || a >= b) ? 0 : 1;
        next_ = limit_ = slot_ == 0 ? a : b;
        healthy_ = true;
        return CounterLoadStatus::Loaded;
    }

    // Every allocation is new content. Retransmissions retain their original
    // frame and do not call this again.
    bool take(uint32_t& counter) {
        if (!healthy_) return false;
        if (next_ == limit_) {
            if (next_ > UINT32_MAX - kReservationSize) { healthy_ = false; return false; }
            if (!commit(next_ + kReservationSize)) return false;
        }
        counter = next_++;
        hasIssued_ = true;
        return true;
    }

    bool advanceToFloor(uint32_t acknowledgedCounter, uint32_t floor) {
        if (!healthy_ || !hasIssued_ || acknowledgedCounter != next_ - 1 ||
            floor <= acknowledgedCounter || floor - acknowledgedCounter > kMaxFloorAdvance) return false;
        if (floor > UINT32_MAX - kReservationSize) { healthy_ = false; return false; }
        if (!commit(floor + kReservationSize)) return false;
        next_ = floor;
        hasIssued_ = false;
        return true;
    }

    uint32_t limit() const { return limit_; }

private:
    bool commit(uint32_t limit) {
        uint8_t bytes[kReserveSize];
        encodeReserve(limit, bytes);
        const uint8_t nextSlot = slot_ ^ 1;
        const size_t address = nextSlot == 0 ? kReserveA : kReserveB;
        // CRC is last. The other valid slot covers every counter already used.
        for (size_t i = 0; i < sizeof(bytes); ++i) storage_.update(address + i, bytes[i]);
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            if (storage_.read(address + i) != bytes[i]) { healthy_ = false; return false; }
        }
        slot_ = nextSlot;
        limit_ = limit;
        return true;
    }

    Storage& storage_;
    uint32_t next_ = 0;
    uint32_t limit_ = 0;
    uint8_t slot_ = 1;
    bool healthy_ = false;
    bool hasIssued_ = false;
};

}  // namespace security_storage
}  // namespace node
}  // namespace radiosensors
