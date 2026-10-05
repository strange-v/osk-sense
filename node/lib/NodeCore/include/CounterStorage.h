#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "NodeStorage.h"

namespace radiosensors {
namespace node {
namespace storage {

constexpr size_t kSetCountSlotSize = 16;
constexpr size_t kSetCountSlotA = kProfileAreaStart;
constexpr size_t kSetCountSlotB = kSetCountSlotA + kSetCountSlotSize;
constexpr size_t kCounterRingStart = kSetCountSlotB + kSetCountSlotSize;
constexpr size_t kCounterRecordSize = 5;
constexpr size_t kCounterRecordCount = 32;
static_assert(
    kCounterRingStart + kCounterRecordSize * kCounterRecordCount == kEepromSize,
    "counter EEPROM layout must fill the ATtiny3224 EEPROM exactly");

enum class SetCountStatus : uint8_t {
    Pending = 1,
    Applied = 2,
};

struct SetCountResult {
    uint8_t generation = 0;
    SetCountStatus status = SetCountStatus::Pending;
    uint16_t commandId = 0;
    uint32_t oldCount = 0;
    uint32_t appliedCount = 0;
};

inline bool encodeSetCountResult(
    const SetCountResult& value, uint8_t* output, const size_t capacity) {
    if (output == nullptr || capacity < kSetCountSlotSize) return false;
    const uint8_t status = static_cast<uint8_t>(value.status);
    if (status != static_cast<uint8_t>(SetCountStatus::Pending) &&
        status != static_cast<uint8_t>(SetCountStatus::Applied)) {
        return false;
    }
    memset(output, 0, kSetCountSlotSize);
    output[0] = 0xC7;
    output[1] = 1;
    output[2] = value.generation;
    output[3] = status;
    write16(output + 4, value.commandId);
    write32(output + 6, value.oldCount);
    write32(output + 10, value.appliedCount);
    write16(output + 14, crc16Ccitt(output, 14));
    return true;
}

inline bool decodeSetCountResult(
    const uint8_t* input, const size_t size, SetCountResult& value) {
    if (input == nullptr || size != kSetCountSlotSize || input[0] != 0xC7 ||
        input[1] != 1 || read16(input + 14) != crc16Ccitt(input, 14)) {
        return false;
    }
    if (input[3] != static_cast<uint8_t>(SetCountStatus::Pending) &&
        input[3] != static_cast<uint8_t>(SetCountStatus::Applied)) {
        return false;
    }
    value.generation = input[2];
    value.status = static_cast<SetCountStatus>(input[3]);
    value.commandId = read16(input + 4);
    value.oldCount = read32(input + 6);
    value.appliedCount = read32(input + 10);
    return true;
}

template <typename Storage>
class SetCountStore {
public:
    explicit SetCountStore(Storage& storage) : storage_(storage) {}

    bool load(SetCountResult& value) {
        uint8_t first[kSetCountSlotSize];
        uint8_t second[kSetCountSlotSize];
        SetCountResult a{};
        SetCountResult b{};
        readBlock(storage_, kSetCountSlotA, first, sizeof(first));
        readBlock(storage_, kSetCountSlotB, second, sizeof(second));
        const bool validA = decodeSetCountResult(first, sizeof(first), a);
        const bool validB = decodeSetCountResult(second, sizeof(second), b);
        initialized_ = true;
        hasValue_ = validA || validB;
        if (!hasValue_) {
            currentSlot_ = 1;
            currentGeneration_ = 0;
            return false;
        }
        if (validA && (!validB || generationIsNewer(a.generation, b.generation))) {
            value = a;
            currentSlot_ = 0;
        } else {
            value = b;
            currentSlot_ = 1;
        }
        currentGeneration_ = value.generation;
        return true;
    }

    bool save(SetCountResult& value) {
        if (!initialized_) {
            SetCountResult ignored{};
            load(ignored);
        }
        value.generation = hasValue_
            ? static_cast<uint8_t>(currentGeneration_ + 1U)
            : 0;
        uint8_t bytes[kSetCountSlotSize];
        if (!encodeSetCountResult(value, bytes, sizeof(bytes))) return false;
        const uint8_t nextSlot = static_cast<uint8_t>(currentSlot_ ^ 1U);
        const size_t address = nextSlot == 0 ? kSetCountSlotA : kSetCountSlotB;
        storage_.update(address, 0);
        for (size_t index = 1; index < sizeof(bytes); ++index) {
            storage_.update(address + index, bytes[index]);
        }
        storage_.update(address, bytes[0]);
        currentSlot_ = nextSlot;
        currentGeneration_ = value.generation;
        hasValue_ = true;
        return true;
    }

private:
    Storage& storage_;
    bool initialized_ = false;
    bool hasValue_ = false;
    uint8_t currentSlot_ = 1;
    uint8_t currentGeneration_ = 0;
};

inline bool counterSequenceIsNewer(
    const uint8_t candidate, const uint8_t current) {
    const uint16_t delta = candidate >= current
        ? candidate - current
        : static_cast<uint16_t>(candidate) + 255U - current;
    return delta != 0 && delta < 128U;
}

template <typename Storage>
class CounterStore {
public:
    explicit CounterStore(Storage& storage) : storage_(storage) {}

    bool load(uint32_t& count) {
        bool found = false;
        uint8_t newestSequence = 0;
        uint8_t newestSlot = 0;
        uint32_t newestCount = 0;
        for (uint8_t slot = 0; slot < kCounterRecordCount; ++slot) {
            const size_t address = kCounterRingStart + slot * kCounterRecordSize;
            const uint8_t sequence = storage_.read(address);
            if (sequence == 0xFFU) continue;
            uint8_t bytes[4];
            readBlock(storage_, address + 1, bytes, sizeof(bytes));
            if (!found || counterSequenceIsNewer(sequence, newestSequence)) {
                found = true;
                newestSequence = sequence;
                newestSlot = slot;
                newestCount = read32(bytes);
            }
        }
        initialized_ = true;
        hasValue_ = found;
        if (!found) {
            currentSlot_ = static_cast<uint8_t>(kCounterRecordCount - 1);
            currentSequence_ = 0;
            currentCount_ = 0;
            count = 0;
            return false;
        }
        currentSlot_ = newestSlot;
        currentSequence_ = newestSequence;
        currentCount_ = newestCount;
        count = newestCount;
        return true;
    }

    bool save(const uint32_t count) {
        if (!initialized_) {
            uint32_t ignored = 0;
            load(ignored);
        }
        if (hasValue_ && count == currentCount_) return true;
        const uint8_t nextSlot = static_cast<uint8_t>(
            (currentSlot_ + 1U) % kCounterRecordCount);
        const uint8_t nextSequence = !hasValue_
            ? 0
            : (currentSequence_ == 0xFEU
                ? 0
                : static_cast<uint8_t>(currentSequence_ + 1U));
        const size_t address =
            kCounterRingStart + nextSlot * kCounterRecordSize;
        uint8_t bytes[4];
        write32(bytes, count);
        // The destination is not invalidated first: until the sequence is
        // committed it holds the oldest one in the ring, which load() never
        // selects, and a second write would double the wear on this byte.
        for (uint8_t index = 0; index < sizeof(bytes); ++index) {
            storage_.update(address + 1U + index, bytes[index]);
        }
        storage_.update(address, nextSequence);
        currentSlot_ = nextSlot;
        currentSequence_ = nextSequence;
        currentCount_ = count;
        hasValue_ = true;
        return true;
    }

private:
    Storage& storage_;
    bool initialized_ = false;
    bool hasValue_ = false;
    uint8_t currentSlot_ = static_cast<uint8_t>(kCounterRecordCount - 1);
    uint8_t currentSequence_ = 0;
    uint32_t currentCount_ = 0;
};

enum class SetCountOutcome : uint8_t {
    Applied,
    Redelivered,
    StorageFailure,
};

// Applies SET_COUNT in three recoverable steps: a Pending result, the new
// count in the ring, then the Applied result. A command whose ID matches the
// stored result is a redelivery and leaves the count alone.
template <typename Storage>
class SetCountJournal {
public:
    SetCountJournal(CounterStore<Storage>& counter, SetCountStore<Storage>& results)
        : counter_(counter), results_(results) {}

    // Finishes an apply interrupted by a reset. Call before counting pulses.
    void recover(uint32_t& count) {
        hasResult_ = results_.load(last_);
        if (hasResult_ && last_.status == SetCountStatus::Pending) {
            completePending(count);
        }
    }

    SetCountOutcome apply(
        const uint16_t commandId, const uint32_t requested, uint32_t& count,
        SetCountResult& result) {
        if (hasResult_ && commandId != 0 && last_.commandId == commandId) {
            if (last_.status == SetCountStatus::Pending &&
                !completePending(count)) {
                return SetCountOutcome::StorageFailure;
            }
            result = last_;
            return SetCountOutcome::Redelivered;
        }
        SetCountResult next{};
        next.status = SetCountStatus::Pending;
        next.commandId = commandId;
        next.oldCount = count;
        next.appliedCount = requested;
        if (!results_.save(next)) return SetCountOutcome::StorageFailure;
        last_ = next;
        hasResult_ = true;
        if (!completePending(count)) return SetCountOutcome::StorageFailure;
        result = last_;
        return SetCountOutcome::Applied;
    }

    // Commissioning starts a new command ID space, so an ID recorded under a
    // previous installation must not look like a redelivery.
    void forgetCommandIds(const uint32_t count) {
        if (!hasResult_ || last_.commandId == 0) return;
        SetCountResult cleared{};
        cleared.status = SetCountStatus::Applied;
        cleared.oldCount = count;
        cleared.appliedCount = count;
        if (results_.save(cleared)) last_ = cleared;
    }

private:
    bool completePending(uint32_t& count) {
        count = last_.appliedCount;
        if (!counter_.save(count)) return false;
        last_.status = SetCountStatus::Applied;
        return results_.save(last_);
    }

    CounterStore<Storage>& counter_;
    SetCountStore<Storage>& results_;
    SetCountResult last_{};
    bool hasResult_ = false;
};

}  // namespace storage
}  // namespace node
}  // namespace radiosensors
