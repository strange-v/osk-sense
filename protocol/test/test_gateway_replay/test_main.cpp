#include <GatewayReplay.h>
#include <JoinRequest.h>
#include <unity.h>
#include <string.h>
#include <algorithm>
#include <initializer_list>

using namespace radiosensors::replay;
namespace {
struct PowerCut {};
struct Memory : BlobStorage {
    uint8_t blob[kSnapshotSize]{};
    size_t storedSize = 0;
    size_t budget = kSnapshotSize;
    unsigned writes = 0;
    bool writeResult = true, readError = false, corruptReadback = false;
    bool cutBeforeCommit = false, cutAfterCommit = false;
    bool invalidBlob = false;
    ReadStatus read(uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (readError) return ReadStatus::Error;
        if (invalidBlob) return ReadStatus::Invalid;
        if (!storedSize) return ReadStatus::Missing;
        size = storedSize;
        if (size > capacity) return ReadStatus::Error;
        memcpy(output, blob, size);
        if (corruptReadback) output[0] ^= 1;
        return ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        ++writes;
        if (cutBeforeCommit) throw PowerCut{};
        memcpy(blob, data, std::min(size, budget));
        storedSize = size;
        if (cutAfterCommit) throw PowerCut{};
        return writeResult;
    }
};
struct Random : RandomSource {
    unsigned calls = 0;
    bool success = true;
    bool fill(uint8_t* output, size_t size) override {
        ++calls;
        for (size_t i = 0; i < size; ++i) output[i] = calls + i;
        return success;
    }
};
void action(Action expected, const Decision& decision) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(decision.action));
}
void seed(Memory& memory, const Snapshot& snapshot, uint32_t generation = 10) {
    TEST_ASSERT_TRUE(encode(snapshot, generation, memory.blob, kSnapshotSize));
    memory.storedSize = kSnapshotSize;
}
uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
    }
    return ~crc;
}
void repairCrc(uint8_t* bytes) {
    radiosensors::protocol::writeUint32Le(bytes + kSnapshotSize - 4, crc32(bytes, kSnapshotSize - 4));
}
}
void setUp() {}
void tearDown() {}

void test_layout_corruption_and_strict_fields() {
    Snapshot snapshot;
    snapshot.records[0] = Record{RecordState::Paired, 0};
    snapshot.records[63] = Record{RecordState::Bound, 0x12345678};
    uint8_t bytes[kSnapshotSize], changed[kSnapshotSize];
    TEST_ASSERT_TRUE(encode(snapshot, 0xAABBCCDD, bytes, sizeof(bytes)));
    const uint8_t header[] = {'R','S','R','B',3,0,0xDD,0xCC,0xBB,0xAA,0x20,1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(header, bytes, sizeof(header));
    TEST_ASSERT_EQUAL_HEX8(1, bytes[12]);
    TEST_ASSERT_EQUAL_HEX8(0x80, bytes[19]);
    TEST_ASSERT_EQUAL_HEX8(0x80, bytes[27]);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, radiosensors::protocol::readUint32Le(bytes + 280));
    Snapshot result;
    uint32_t generation = 0;
    TEST_ASSERT_TRUE(decode(bytes, sizeof(bytes), result, generation));
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDD, generation);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        memcpy(changed, bytes, sizeof(bytes));
        changed[i] ^= 1;
        TEST_ASSERT_FALSE(decode(changed, sizeof(changed), result, generation));
    }
    TEST_ASSERT_FALSE(decode(bytes, sizeof(bytes) - 1, result, generation));
    TEST_ASSERT_FALSE(encode(snapshot, 0, changed, sizeof(changed) - 1));
    memcpy(changed, bytes, sizeof(bytes));
    changed[20] = 2;
    repairCrc(changed);
    TEST_ASSERT_FALSE(decode(changed, sizeof(changed), result, generation));
    memcpy(changed, bytes, sizeof(bytes));
    changed[28] = 1;
    repairCrc(changed);
    TEST_ASSERT_FALSE(decode(changed, sizeof(changed), result, generation));
    snapshot.records[1].state = static_cast<RecordState>(9);
    TEST_ASSERT_FALSE(encode(snapshot, 0, bytes, sizeof(bytes)));
}

void test_paired_first_frame_duplicate_replay_and_single_jump_write() {
    Memory memory;
    Store store(memory);
    Random random;
    Guard guard(store, random);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Empty), static_cast<uint8_t>(store.load()));
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
    guard.restart();
    TEST_ASSERT_TRUE(guard.initializeFreshKeys(0));
    TEST_ASSERT_FALSE(guard.initializeFreshKeys(0));
    action(Action::Accept, guard.inspect(0, 5000));
    TEST_ASSERT_EQUAL_UINT32(5255, store.snapshot().records[0].upperBound);
    const unsigned writes = memory.writes;
    action(Action::Duplicate, guard.inspect(0, 5000));
    Decision replay = guard.inspect(0, 4999);
    action(Action::CounterFloor, replay);
    TEST_ASSERT_EQUAL_UINT32(5001, replay.floor);
    action(Action::Accept, guard.inspect(0, 5001));
    TEST_ASSERT_EQUAL_UINT(writes, memory.writes);
    action(Action::Accept, guard.inspect(0, 1000000));
    TEST_ASSERT_EQUAL_UINT(writes + 1, memory.writes);
    TEST_ASSERT_EQUAL_UINT32(1000255, store.snapshot().records[0].upperBound);
    action(Action::InvalidSlot, guard.inspect(kNodeSlots, 0));
    TEST_ASSERT_FALSE(guard.initializeFreshKeys(kNodeSlots));
    TEST_ASSERT_FALSE(guard.forget(kNodeSlots));
}

void test_restart_floor_and_node_isolation() {
    Memory memory;
    Snapshot snapshot;
    snapshot.records[0] = Record{RecordState::Bound, 200};
    snapshot.records[1] = Record{RecordState::Paired, 0};
    seed(memory, snapshot);
    Store store(memory);
    store.load();
    Random random;
    Guard guard(store, random);
    guard.restart();
    Decision replay = guard.inspect(0, 199);
    action(Action::CounterFloor, replay);
    TEST_ASSERT_EQUAL_UINT32(201, replay.floor);
    action(Action::Accept, guard.inspect(1, 0, 1));
    action(Action::Accept, guard.inspect(0, 201, 2));
    TEST_ASSERT_EQUAL_UINT32(202, store.snapshot().records[0].upperBound);
    Store restarted(memory);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Loaded), static_cast<uint8_t>(restarted.load()));
    Guard reboot(restarted, random);
    reboot.restart();
    action(Action::CounterFloor, reboot.inspect(0, 201));
    TEST_ASSERT_EQUAL_UINT32(203, reboot.inspect(0, 201).floor);
    action(Action::CounterFloor, reboot.inspect(1, 0));
    TEST_ASSERT_EQUAL_UINT(2, memory.writes);
}

void test_restore_challenge_repeats_and_restart_invalidates_it() {
    Memory memory;
    Store store(memory);
    store.load();
    Random random;
    Guard guard(store, random);
    guard.restart();
    Decision challenge = guard.inspect(0, 90);
    action(Action::Challenge, challenge);
    Decision repeated = guard.inspect(0, 91);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(challenge.challenge, repeated.challenge, kChallengeSize);
    TEST_ASSERT_EQUAL_UINT(1, random.calls);
    guard.restart();
    Decision stale = guard.inspect(0, 92, 256, challenge.challenge);
    action(Action::Challenge, stale);
    TEST_ASSERT_NOT_EQUAL(challenge.challenge[0], stale.challenge[0]);
    action(Action::Challenge, guard.inspect(1, 92, 256, stale.challenge));
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
    action(Action::Accept, guard.inspect(0, 93, 256, stale.challenge));
    TEST_ASSERT_EQUAL_UINT(1, memory.writes);
    action(Action::Duplicate, guard.inspect(0, 93, 256, stale.challenge));
    store.load();
    guard.restart();
    action(Action::CounterFloor, guard.inspect(0, 93, 256, stale.challenge));
}

void test_activation_commit_precedes_accept_and_survives_reboot() {
    Memory memory;
    Snapshot empty;
    seed(memory, empty);
    Store store(memory);
    store.load();
    Random random;
    Guard guard(store, random);
    guard.restart();
    Decision challenge = guard.inspect(0, 100);
    action(Action::Accept, guard.inspect(0, 101, 64, challenge.challenge));
    TEST_ASSERT_EQUAL_UINT32(164, store.snapshot().records[0].upperBound);
    store.load();
    guard.restart();
    Decision lostAck = guard.inspect(0, 101, 64, challenge.challenge);
    action(Action::CounterFloor, lostAck);
    TEST_ASSERT_EQUAL_UINT32(165, lostAck.floor);
    action(Action::Accept, guard.inspect(0, 165));
    seed(memory, empty, 20);
    store.load();
    guard.restart();
    action(Action::Challenge, guard.inspect(0, 165));
}

void test_every_torn_write_and_uncertain_readback_fail_closed() {
    Snapshot baseline;
    baseline.records[0] = Record{RecordState::Bound, 100};
    for (bool writeResult : {false, true}) for (size_t cut = 0; cut <= kSnapshotSize; ++cut) {
        Memory memory;
        seed(memory, baseline);
        Store store(memory);
        store.load();
        Random random;
        Guard guard(store, random);
        guard.restart();
        memory.budget = cut;
        memory.writeResult = writeResult;
        Snapshot expected = baseline;
        expected.records[0].upperBound = 1255;
        uint8_t bytes[kSnapshotSize];
        TEST_ASSERT_TRUE(encode(expected, 11, bytes, sizeof(bytes)));
        const Decision attempt = guard.inspect(0, 1000);
        const bool committed = writeResult && memcmp(bytes, memory.blob, sizeof(bytes)) == 0;
        action(committed ? Action::Accept : Action::StorageError, attempt);
        TEST_ASSERT_EQUAL_UINT32(committed ? 1255 : 100, store.snapshot().records[0].upperBound);
        action(committed ? Action::Duplicate : Action::StorageError, guard.inspect(0, 1000));
        TEST_ASSERT_EQUAL_UINT(1, memory.writes);
        memory.budget = kSnapshotSize;
        memory.writeResult = true;
        store.load();
        guard.restart();
        Decision old = guard.inspect(0, 100);
        TEST_ASSERT_TRUE(old.action == Action::CounterFloor || old.action == Action::Challenge);
    }
    Memory memory;
    seed(memory, baseline);
    Store store(memory);
    store.load();
    Random random;
    Guard guard(store, random);
    guard.restart();
    memory.corruptReadback = true;
    action(Action::StorageError, guard.inspect(0, 1000));
    TEST_ASSERT_FALSE(store.writable());
    memory.corruptReadback = false;
    store.load();
    guard.restart();
    action(Action::CounterFloor, guard.inspect(0, 1000));
}

void test_damaged_or_missing_blob_requires_activation() {
    Memory memory;
    Snapshot snapshot;
    snapshot.records[0] = Record{RecordState::Bound, 1000};
    seed(memory, snapshot);
    memory.blob[100] ^= 1;
    Store store(memory);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Unactivated), static_cast<uint8_t>(store.load()));
    Random random;
    Guard guard(store, random);
    guard.restart();
    action(Action::Challenge, guard.inspect(0, 500));
    memory.storedSize = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Empty), static_cast<uint8_t>(store.load()));
    guard.restart();
    action(Action::Challenge, guard.inspect(0, 500));
    memory.invalidBlob = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Unactivated), static_cast<uint8_t>(store.load()));
    TEST_ASSERT_TRUE(store.writable());
    guard.restart();
    action(Action::Challenge, guard.inspect(0, 500));
    memory.invalidBlob = false;
    memory.readError = true;
    store.load();
    guard.restart();
    action(Action::StorageError, guard.inspect(0, 500));
}

void test_window_clamping_saturation_and_exhaustion() {
    for (uint16_t window : {uint16_t(0), uint16_t(1), uint16_t(256), uint16_t(65535)}) {
        Memory memory;
        Snapshot snapshot;
        snapshot.records[0] = Record{RecordState::Paired, 0};
        seed(memory, snapshot);
        Store store(memory);
        store.load();
        Random random;
        Guard guard(store, random);
        guard.restart();
        action(Action::Accept, guard.inspect(0, 100, window));
        const uint16_t clamped = window < 1 ? 1 : window > 256 ? 256 : window;
        TEST_ASSERT_EQUAL_UINT32(100 + clamped - 1, store.snapshot().records[0].upperBound);
        action(Action::Accept, guard.inspect(0, UINT32_MAX - 1, 256));
        TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, store.snapshot().records[0].upperBound);
        action(Action::Accept, guard.inspect(0, UINT32_MAX));
        action(Action::Exhausted, guard.inspect(0, 0));
        store.load();
        guard.restart();
        action(Action::Exhausted, guard.inspect(0, UINT32_MAX - 1));
        TEST_ASSERT_FALSE(guard.initializeFreshKeys(0));
    }
}

void test_generation_wrap_and_fresh_key_reset() {
    Memory memory;
    Snapshot snapshot;
    snapshot.records[0] = Record{RecordState::Bound, 100};
    seed(memory, snapshot, UINT32_MAX);
    Store store(memory);
    store.load();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, store.generation());
    Random random;
    Guard guard(store, random);
    guard.restart();
    TEST_ASSERT_FALSE(guard.initializeFreshKeys(0));
    TEST_ASSERT_TRUE(guard.forget(0));
    TEST_ASSERT_EQUAL_UINT32(0, store.generation());
    TEST_ASSERT_TRUE(guard.initializeFreshKeys(0));
    action(Action::Accept, guard.inspect(0, 0));
    TEST_ASSERT_EQUAL_UINT32(2, store.generation());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Loaded), static_cast<uint8_t>(store.load()));
    guard.restart();
    action(Action::CounterFloor, guard.inspect(0, 0));
}

void test_atomic_replacement_power_cut_before_and_after_commit() {
    for (bool before : {false, true}) {
        Memory memory;
        Snapshot baseline;
        baseline.records[0] = Record{RecordState::Bound, 100};
        seed(memory, baseline);
        Store store(memory);
        store.load();
        Random random;
        Guard guard(store, random);
        guard.restart();
        memory.cutBeforeCommit = before;
        memory.cutAfterCommit = !before;
        bool interrupted = false;
        try { guard.inspect(0, 1000); } catch (const PowerCut&) { interrupted = true; }
        TEST_ASSERT_TRUE(interrupted);
        TEST_ASSERT_EQUAL_UINT32(100, store.snapshot().records[0].upperBound);
        memory.cutBeforeCommit = memory.cutAfterCommit = false;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadStatus::Loaded), static_cast<uint8_t>(store.load()));
        TEST_ASSERT_EQUAL_UINT32(before ? 100 : 1255, store.snapshot().records[0].upperBound);
        guard.restart();
        const Decision replay = guard.inspect(0, 100);
        action(Action::CounterFloor, replay);
        TEST_ASSERT_EQUAL_UINT32(before ? 101 : 1256, replay.floor);
        action(before ? Action::Accept : Action::CounterFloor, guard.inspect(0, 1000));
    }
}

void test_random_failure_cannot_activate() {
    Memory memory;
    Store store(memory);
    store.load();
    Random random;
    random.success = false;
    Guard guard(store, random);
    guard.restart();
    uint8_t forged[kChallengeSize]{};
    action(Action::StorageError, guard.inspect(0, 0, 256, forged));
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
    random.success = true;
    action(Action::Challenge, guard.inspect(0, 0, 256, forged));
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
}

void test_identical_snapshot_is_noop_and_invalid_candidate_changes_nothing() {
    Memory memory;
    Snapshot snapshot;
    seed(memory, snapshot);
    Store store(memory);
    store.load();
    TEST_ASSERT_TRUE(store.save(snapshot));
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
    const uint32_t generation = store.generation();
    snapshot.records[0] = Record{RecordState::Paired, 1};
    TEST_ASSERT_FALSE(store.save(snapshot));
    TEST_ASSERT_EQUAL_UINT32(generation, store.generation());
    TEST_ASSERT_TRUE(store.writable());
    TEST_ASSERT_EQUAL_UINT(0, memory.writes);
}

void test_adaptive_window_trailing_day_and_clock_wrap() {
    AcceptanceHistory history;
    history.begin(0);
    TEST_ASSERT_EQUAL_UINT16(256, history.window(0));
    history.accepted(1);
    history.accepted(2);
    history.accepted(100);
    TEST_ASSERT_EQUAL_UINT16(256, history.window(86399));
    TEST_ASSERT_EQUAL_UINT16(3, history.window(86400));
    TEST_ASSERT_EQUAL_UINT16(2, history.window(86401));
    TEST_ASSERT_EQUAL_UINT16(1, history.window(86500));
    for (unsigned i = 0; i < 300; ++i) history.accepted(86501);
    TEST_ASSERT_EQUAL_UINT16(256, history.window(86501));
    TEST_ASSERT_EQUAL_UINT16(1, history.window(172901));
    TEST_ASSERT_EQUAL_UINT16(256, history.window(0));
    const uint64_t base = UINT32_MAX - 86399ULL;
    history.begin(base);
    history.accepted(base + 1);
    history.accepted(base + 86400);
    TEST_ASSERT_EQUAL_UINT16(2, history.window(base + 86400));
    TEST_ASSERT_EQUAL_UINT16(1, history.window(base + 86401));
    // A gap exceeding the timestamp modulus cannot resurrect expired history.
    TEST_ASSERT_EQUAL_UINT16(1, history.window(base + 0x100000000ULL + 86401));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_layout_corruption_and_strict_fields);
    RUN_TEST(test_paired_first_frame_duplicate_replay_and_single_jump_write);
    RUN_TEST(test_restart_floor_and_node_isolation);
    RUN_TEST(test_restore_challenge_repeats_and_restart_invalidates_it);
    RUN_TEST(test_activation_commit_precedes_accept_and_survives_reboot);
    RUN_TEST(test_every_torn_write_and_uncertain_readback_fail_closed);
    RUN_TEST(test_damaged_or_missing_blob_requires_activation);
    RUN_TEST(test_window_clamping_saturation_and_exhaustion);
    RUN_TEST(test_generation_wrap_and_fresh_key_reset);
    RUN_TEST(test_atomic_replacement_power_cut_before_and_after_commit);
    RUN_TEST(test_random_failure_cannot_activate);
    RUN_TEST(test_identical_snapshot_is_noop_and_invalid_candidate_changes_nothing);
    RUN_TEST(test_adaptive_window_trailing_day_and_clock_wrap);
    return UNITY_END();
}
