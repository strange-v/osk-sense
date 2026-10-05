#include <RegistryPairing.h>
#include <unity.h>
#include <string.h>
#include <vector>

namespace s = radiosensors::security;
namespace t = s::pairing;
namespace f = s::frames;
namespace r = radiosensors::replay;
namespace n = radiosensors::registry;
namespace {
constexpr s::Transport up{100,0,0}, down{0,100,0};
const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
osk::crypto::Cmac factoryMac;
struct Memory : r::BlobStorage {
    std::vector<uint8_t> bytes;
    unsigned writes = 0;
    bool failWrite = false, failRead = false, loseWrite = false, failReadback = false;
    r::ReadStatus read(uint8_t* out, size_t capacity, size_t& size) override {
        size = 0;
        if (failRead) return r::ReadStatus::Error;
        if (bytes.empty()) return r::ReadStatus::Missing;
        if (bytes.size() > capacity) return r::ReadStatus::Invalid;
        size = bytes.size(); memcpy(out, bytes.data(), size); return r::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        ++writes;
        if (failWrite) return false;
        if (!loseWrite) bytes.assign(data, data + size);
        if (failReadback) failRead = true;
        return true;
    }
};
struct Random : r::RandomSource {
    unsigned calls = 0;
    bool fill(uint8_t* out, size_t size) override {
        ++calls;
        for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(calls + i);
        return true;
    }
};
struct Fixture {
    Memory registryMemory, boundMemory;
    Random random;
    n::NodeRegistry nodes;
    n::AtomicRegistryStore store{registryMemory};
    r::Store bounds{boundMemory};
    r::Guard guard{bounds, random};
    n::PairingAdapter adapter{nodes, store, bounds, guard, random};
    f::JoinRequest request;
    uint8_t wire[33]{}, accept[35]{}, proof[27]{};
    Fixture() {
        for (uint8_t i = 0; i < 10; ++i) request.identity.uid[i] = i + 1;
        request.identity.requestNonce = 0xFEDCBA9800000001ULL;
        request.profileId = 6; request.firmware = {1,2,3}; request.maxPowerLevel = 31;
        store.load(nodes); bounds.load(); guard.restart(); seal();
    }
    void seal() { TEST_ASSERT_TRUE(f::sealJoinRequest(factoryMac, up, request, wire, sizeof(wire))); }
    t::Status join() {
        return adapter.request(factoryMac, request.identity.uid, up, wire, sizeof(wire), 123, down, accept, sizeof(accept));
    }
    t::Status confirm(uint8_t* out) {
        const auto* node = nodes.findByUid(request.identity.uid);
        TEST_ASSERT_NOT_NULL(node);
        t::Record pairing; uint32_t generation = 0;
        TEST_ASSERT_TRUE(t::decode(node->pairing, sizeof(node->pairing), pairing, generation));
        osk::crypto::Cmac mac; osk::crypto::cmacInit(mac, pairing.keys.authentication);
        uint8_t salt[8]; memcpy(salt, pairing.accept + 19, 8);
        const s::Transport transport{100,node->nodeId,0};
        TEST_ASSERT_TRUE(f::sealJoinProof(mac, transport, request.identity, salt, false, proof, sizeof(proof)));
        return adapter.confirm(node->nodeId, transport, proof, sizeof(proof), out, 25);
    }
    void restart() {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::LoadStatus::Loaded), static_cast<uint8_t>(store.load(nodes)));
        bounds.load(); guard.restart();
    }
};
void status(t::Status expected, t::Status actual) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual));
}
}
void setUp() { osk::crypto::cmacInit(factoryMac, factory); }
void tearDown() {}

void test_durable_reply_keys_and_repeat_confirmation() {
    Fixture x; status(t::Status::Accept, x.join());
    TEST_ASSERT_EQUAL_UINT(1, x.nodes.size());
    uint8_t expected[35]; memcpy(expected, x.accept, sizeof(expected));
    x.nodes.rename(1, "room", 4); TEST_ASSERT_TRUE(x.store.save(x.nodes));
    x.restart(); status(t::Status::RepeatedAccept, x.join());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, x.accept, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT(1, x.random.calls); TEST_ASSERT_EQUAL_UINT(2, x.registryMemory.writes);
    const auto* node = x.nodes.findByNodeId(1);
    t::Record pairing; uint32_t generation = 0;
    TEST_ASSERT_TRUE(t::decode(node->pairing, sizeof(node->pairing), pairing, generation));
    TEST_ASSERT_EQUAL_HEX32(0xFEDCBA98, radiosensors::protocol::readUint32Le(pairing.request + 15));
    s::Keys keys; uint8_t slot = 255;
    TEST_ASSERT_FALSE(x.adapter.activeKeys(1, keys, slot));
    uint8_t complete[25]; status(t::Status::Complete, x.confirm(complete));
    TEST_ASSERT_TRUE(x.adapter.activeKeys(1, keys, slot));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pairing.keys.encryption, keys.encryption, 16);
    const unsigned bw = x.boundMemory.writes, rw = x.registryMemory.writes;
    x.restart(); uint8_t repeated[25]; status(t::Status::RepeatedComplete, x.confirm(repeated));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(complete, repeated, sizeof(complete));
    TEST_ASSERT_EQUAL_UINT(bw, x.boundMemory.writes); TEST_ASSERT_EQUAL_UINT(rw, x.registryMemory.writes);
    status(t::Status::ActiveNode, x.join());
}
void test_failed_active_commit_preserves_prepared_bound() {
    Fixture x; status(t::Status::Accept, x.join());
    x.registryMemory.failWrite = true; uint8_t complete[25]; memset(complete, 0xAA, sizeof(complete));
    status(t::Status::StorageError, x.confirm(complete));
    for (const auto byte : complete) TEST_ASSERT_EQUAL_HEX8(0xAA, byte);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::RecordState::Paired), static_cast<uint8_t>(x.bounds.snapshot().records[0].state));
    const unsigned writes = x.boundMemory.writes;
    x.registryMemory.failWrite = false;
    status(t::Status::StorageError, x.confirm(complete));
    x.restart(); status(t::Status::Complete, x.confirm(complete));
    TEST_ASSERT_EQUAL_UINT(writes, x.boundMemory.writes);
}
void test_failed_readback_reloads_active_without_bound_reset() {
    Fixture x; status(t::Status::Accept, x.join());
    x.registryMemory.failReadback = true; uint8_t complete[25];
    status(t::Status::StorageError, x.confirm(complete));
    x.registryMemory.failReadback = x.registryMemory.failRead = false; x.restart();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::Action::Accept), static_cast<uint8_t>(x.guard.inspect(0, 42).action));
    const unsigned writes = x.boundMemory.writes;
    status(t::Status::RepeatedComplete, x.confirm(complete));
    TEST_ASSERT_EQUAL_UINT(writes, x.boundMemory.writes);
    TEST_ASSERT_EQUAL_UINT32(297, x.bounds.snapshot().records[0].upperBound);
}
void test_replacing_pending_keys_clears_prepared_bound() {
    Fixture x; status(t::Status::Accept, x.join());
    x.registryMemory.failWrite = true; uint8_t complete[25]; status(t::Status::StorageError, x.confirm(complete));
    x.registryMemory.failWrite = false; x.restart();
    ++x.request.identity.requestNonce; x.seal(); status(t::Status::Accept, x.join());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::RecordState::Absent), static_cast<uint8_t>(x.bounds.snapshot().records[0].state));
    status(t::Status::Complete, x.confirm(complete));
}
void test_slots_survive_record_movement_and_are_cleared_on_reuse() {
    Fixture x; status(t::Status::Accept, x.join()); uint8_t complete[25]; status(t::Status::Complete, x.confirm(complete));
    ++x.request.identity.uid[0]; x.seal(); status(t::Status::Accept, x.join()); status(t::Status::Complete, x.confirm(complete));
    x.guard.inspect(0, 100); x.guard.inspect(1, 200);
    TEST_ASSERT_TRUE(x.nodes.remove(1)); TEST_ASSERT_TRUE(x.store.save(x.nodes)); x.restart();
    s::Keys keys; uint8_t slot = 255;
    TEST_ASSERT_TRUE(x.adapter.activeKeys(2, keys, slot)); TEST_ASSERT_EQUAL_UINT8(1, slot);
    const auto bound = x.bounds.snapshot().records[1].upperBound;
    ++x.request.identity.uid[0]; x.seal(); status(t::Status::Accept, x.join());
    TEST_ASSERT_EQUAL_UINT8(0, x.nodes.findByNodeId(1)->replaySlot);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::RecordState::Absent), static_cast<uint8_t>(x.bounds.snapshot().records[0].state));
    TEST_ASSERT_EQUAL_UINT32(bound, x.bounds.snapshot().records[1].upperBound);
}
void test_corruption_and_readback_failure_block_new_transactions() {
    Fixture x; status(t::Status::Accept, x.join()); x.registryMemory.bytes.back() ^= 1;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::LoadStatus::Invalid), static_cast<uint8_t>(x.store.load(x.nodes)));
    const unsigned writes = x.registryMemory.writes;
    status(t::Status::StorageError, x.join()); TEST_ASSERT_EQUAL_UINT(writes, x.registryMemory.writes);
    TEST_ASSERT_FALSE(x.store.save(x.nodes));
    Fixture y; y.registryMemory.loseWrite = true;
    status(t::Status::StorageError, y.join()); y.registryMemory.loseWrite = false;
    status(t::Status::StorageError, y.join());
}
void test_invalid_tag_and_full_registry_do_not_write() {
    Fixture x; x.wire[32] ^= 1; status(t::Status::InvalidFrame, x.join());
    TEST_ASSERT_EQUAL_UINT(0, x.registryMemory.writes); x.seal();
    for (uint8_t i = 0; i < n::kMaxNodes; ++i) {
        x.request.identity.uid[0] = i; x.seal(); status(t::Status::Accept, x.join());
    }
    TEST_ASSERT_EQUAL_UINT(12560, x.registryMemory.bytes.size());
    x.request.identity.uid[0] = 200; x.seal(); status(t::Status::RegistryFull, x.join());
    TEST_ASSERT_EQUAL_UINT(n::kMaxNodes, x.registryMemory.writes);
}
void test_bound_write_failure_never_activates_or_returns_complete() {
    Fixture x; status(t::Status::Accept, x.join()); uint8_t complete[25];
    memset(complete, 0xAA, sizeof(complete)); x.boundMemory.failWrite = true;
    status(t::Status::StorageError, x.confirm(complete));
    for (const auto byte : complete) TEST_ASSERT_EQUAL_HEX8(0xAA, byte);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(n::NodeState::Pending), static_cast<uint8_t>(x.nodes.findByNodeId(1)->state));
    x.boundMemory.failWrite = false;
    status(t::Status::StorageError, x.confirm(complete));
    x.restart(); status(t::Status::Complete, x.confirm(complete));
}
void test_restored_active_confirmation_does_not_initialize_missing_bounds() {
    Fixture x; status(t::Status::Accept, x.join()); uint8_t complete[25]; status(t::Status::Complete, x.confirm(complete));
    x.boundMemory.bytes.clear(); x.restart();
    const unsigned writes = x.boundMemory.writes;
    status(t::Status::RepeatedComplete, x.confirm(complete));
    TEST_ASSERT_EQUAL_UINT(writes, x.boundMemory.writes);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(r::Action::Challenge), static_cast<uint8_t>(x.guard.inspect(0, 50).action));
}
void test_invalid_registry_slot_ownership_is_rejected() {
    Fixture x; status(t::Status::Accept, x.join());
    ++x.request.identity.uid[0]; x.seal(); status(t::Status::Accept, x.join());
    n::NodeRecord records[2] = {x.nodes.records()[0], x.nodes.records()[1]};
    records[1].replaySlot = records[0].replaySlot;
    n::NodeRegistry decoded; TEST_ASSERT_FALSE(decoded.restore(records, 2));
    records[1] = x.nodes.records()[1]; records[1].deviceUid[1] ^= 1;
    TEST_ASSERT_FALSE(decoded.restore(records, 2));
    records[1] = x.nodes.records()[1]; records[1].state = n::NodeState::Active;
    TEST_ASSERT_FALSE(decoded.restore(records, 2));
}
int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_durable_reply_keys_and_repeat_confirmation);
    RUN_TEST(test_failed_active_commit_preserves_prepared_bound);
    RUN_TEST(test_failed_readback_reloads_active_without_bound_reset);
    RUN_TEST(test_replacing_pending_keys_clears_prepared_bound);
    RUN_TEST(test_slots_survive_record_movement_and_are_cleared_on_reuse);
    RUN_TEST(test_corruption_and_readback_failure_block_new_transactions);
    RUN_TEST(test_invalid_tag_and_full_registry_do_not_write);
    RUN_TEST(test_bound_write_failure_never_activates_or_returns_complete);
    RUN_TEST(test_restored_active_confirmation_does_not_initialize_missing_bounds);
    RUN_TEST(test_invalid_registry_slot_ownership_is_rejected);
    return UNITY_END();
}
