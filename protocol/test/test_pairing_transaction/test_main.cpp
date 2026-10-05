#include <PairingTransaction.h>
#include "../../../node/lib/NodeCore/include/RadioSecurityPairing.h"
#include <unity.h>
#include <string.h>

namespace s = radiosensors::security;
namespace f = s::frames;
namespace t = s::pairing;
namespace r = radiosensors::replay;
namespace p = radiosensors::protocol;
namespace {
const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
constexpr s::Transport up{100,0,0}, down{0,100,0}, confirmUp{100,7,0};
osk::crypto::Cmac factoryMac;
struct Memory : r::BlobStorage {
    uint8_t bytes[t::kSnapshotSize]{};
    size_t stored = 0;
    unsigned writes = 0;
    bool failWrite = false, failRead = false, loseWrite = false;
    r::ReadStatus read(uint8_t* out, size_t capacity, size_t& size) override {
        size = stored;
        if (failRead) return r::ReadStatus::Error;
        if (!stored) return r::ReadStatus::Missing;
        if (capacity < stored) return r::ReadStatus::Invalid;
        memcpy(out, bytes, stored); return r::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        ++writes;
        if (failWrite) return false;
        if (!loseWrite) { memcpy(bytes, data, size); stored = size; }
        return true;
    }
};
struct Random : r::RandomSource {
    unsigned calls = 0;
    bool fail = false;
    bool fill(uint8_t* out, size_t size) override {
        ++calls;
        if (fail) return false;
        for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(16 + i + calls - 1);
        return true;
    }
};
struct Bound : t::BoundInitializer {
    unsigned calls = 0, initializations = 0;
    bool fail = false;
    s::Keys keys{};
    bool ensure(const s::Keys& value) override {
        ++calls;
        if (fail) return false;
        if (initializations) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(keys.encryption, value.encryption, 16);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(keys.authentication, value.authentication, 16);
        } else { keys = value; ++initializations; }
        return true;
    }
};
struct Fixture {
    Memory memory;
    Random random;
    t::Transaction transaction{memory,random};
    f::JoinRequest request{};
    uint8_t wire[f::kJoinRequestSize]{}, accept[f::kJoinAcceptSize]{}, proof[f::kJoinConfirmSize]{};
    Fixture() {
        for (uint8_t i = 0; i < 10; ++i) request.identity.uid[i] = i;
        request.identity.requestNonce = 0x0403020100000001ULL;
        request.profileId = 6; request.firmware = {1,2,3}; request.maxPowerLevel = 31;
        seal(); TEST_ASSERT_TRUE(transaction.load());
    }
    void seal() { TEST_ASSERT_TRUE(f::sealJoinRequest(factoryMac, up, request, wire, sizeof(wire))); }
    t::Status join(uint8_t* output = nullptr, size_t capacity = f::kJoinAcceptSize) {
        return transaction.request(factoryMac, request.identity.uid, up, wire, sizeof(wire), 7,123,down,
                                   output ? output : accept, capacity);
    }
    void confirmWire() {
        osk::crypto::Cmac mac;
        osk::crypto::cmacInit(mac, transaction.record().keys.authentication);
        uint8_t salt[8]; memcpy(salt,accept+19,8);
        TEST_ASSERT_TRUE(f::sealJoinProof(mac,confirmUp,request.identity,salt,false,proof,sizeof(proof)));
    }
};
void status(t::Status expected, t::Status actual) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),static_cast<uint8_t>(actual));
}
}
void setUp() { osk::crypto::cmacInit(factoryMac,factory); }
void tearDown() {}

void test_accept_is_durable_immutable_and_keys_match_node() {
    Fixture x; status(t::Status::Accept,x.join());
    TEST_ASSERT_EQUAL_UINT(1,x.memory.writes);
    s::Keys nodeKeys; osk::crypto::Cmac scratch; s::deriveKeys(factory,x.accept+19,nodeKeys,scratch);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nodeKeys.encryption,x.transaction.record().keys.encryption,16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(nodeKeys.authentication,x.transaction.record().keys.authentication,16);
    t::Transaction restarted(x.memory,x.random); TEST_ASSERT_TRUE(restarted.load());
    uint8_t reply[35];
    status(t::Status::RepeatedAccept,restarted.request(factoryMac,x.request.identity.uid,up,x.wire,sizeof(x.wire),
                                                      9,42,s::Transport{0,100,0x40},reply,sizeof(reply)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(x.accept,reply,sizeof(reply));
    TEST_ASSERT_EQUAL_UINT(1,x.random.calls); TEST_ASSERT_EQUAL_UINT(1,x.memory.writes);
}
void test_new_attempt_retires_old_counter_and_conflicting_nonce() {
    Fixture x; status(t::Status::Accept,x.join());
    uint8_t old[33]; memcpy(old,x.wire,33);
    ++x.request.firmware.patch; x.seal();
    status(t::Status::ConflictingRequest,x.join());
    ++x.request.identity.requestNonce; x.seal(); status(t::Status::Accept,x.join());
    TEST_ASSERT_EQUAL_UINT(2,x.random.calls);
    status(t::Status::RetiredCounter,x.transaction.request(factoryMac,x.request.identity.uid,up,old,33,7,123,down,x.accept,35));
    x.request.identity.requestNonce += (1ULL << 32); x.seal();
    status(t::Status::RetiredCounter,x.join());
    TEST_ASSERT_EQUAL_UINT(2,x.memory.writes);
}
void test_confirm_is_durable_and_repeat_never_initializes_bound() {
    Fixture x; Bound bound; uint8_t complete[25], again[25];
    status(t::Status::Accept,x.join()); x.confirmWire();
    status(t::Status::Complete,x.transaction.confirm(confirmUp,x.proof,27,bound,complete,25));
    TEST_ASSERT_EQUAL_UINT(1,bound.initializations); TEST_ASSERT_EQUAL_UINT(2,x.memory.writes);
    t::Transaction reboot(x.memory,x.random); TEST_ASSERT_TRUE(reboot.load());
    bound.fail = true;
    status(t::Status::RepeatedComplete,reboot.confirm(confirmUp,x.proof,27,bound,again,25));
    TEST_ASSERT_EQUAL_UINT(1,bound.calls); TEST_ASSERT_EQUAL_HEX8_ARRAY(complete,again,25);
    osk::crypto::Cmac mac; osk::crypto::cmacInit(mac,bound.keys.authentication);
    f::JoinIdentity decoded;
    uint8_t salt[8]; memcpy(salt,x.accept+19,8);
    TEST_ASSERT_TRUE(f::openJoinProof(mac,s::Transport{7,100,0},again,25,salt,true,decoded));
    status(t::Status::ActiveNode,reboot.request(factoryMac,x.request.identity.uid,up,x.wire,33,7,123,down,x.accept,35));
}
void test_failed_activation_commit_retries_bound_idempotently() {
    Fixture x; Bound bound; uint8_t complete[25]; memset(complete,0xA5,25);
    status(t::Status::Accept,x.join()); x.confirmWire(); x.memory.failWrite = true;
    status(t::Status::StorageError,x.transaction.confirm(confirmUp,x.proof,27,bound,complete,25));
    TEST_ASSERT_EQUAL_HEX8(0xA5,complete[0]); TEST_ASSERT_EQUAL_UINT(1,bound.initializations);
    x.memory.failWrite = false; TEST_ASSERT_TRUE(x.transaction.load());
    status(t::Status::Complete,x.transaction.confirm(confirmUp,x.proof,27,bound,complete,25));
    TEST_ASSERT_EQUAL_UINT(2,bound.calls); TEST_ASSERT_EQUAL_UINT(1,bound.initializations);
}
void test_no_output_without_successful_write_and_exact_readback() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture x; uint8_t reply[35]; memset(reply,0xA5,35);
        x.memory.failWrite = mode == 0; x.memory.failRead = mode == 1; x.memory.loseWrite = mode == 2;
        status(t::Status::StorageError,x.join(reply));
        for (uint8_t b : reply) TEST_ASSERT_EQUAL_HEX8(0xA5,b);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(t::State::Empty),static_cast<uint8_t>(x.transaction.record().state));
        x.memory.failWrite = x.memory.failRead = x.memory.loseWrite = false;
        status(t::Status::StorageError,x.join(reply));
        TEST_ASSERT_TRUE(x.transaction.load());
        status(mode == 1 ? t::Status::RepeatedAccept : t::Status::Accept,x.join(reply));
    }
}
void test_activation_committed_without_readback_recovers_as_active() {
    Fixture x; Bound bound; uint8_t complete[25]; memset(complete,0xA5,25);
    status(t::Status::Accept,x.join()); x.confirmWire(); x.memory.failRead = true;
    status(t::Status::StorageError,x.transaction.confirm(confirmUp,x.proof,27,bound,complete,25));
    TEST_ASSERT_EQUAL_HEX8(0xA5,complete[0]);
    x.memory.failRead = false; TEST_ASSERT_TRUE(x.transaction.load()); bound.fail = true;
    status(t::Status::RepeatedComplete,x.transaction.confirm(confirmUp,x.proof,27,bound,complete,25));
    TEST_ASSERT_EQUAL_UINT(1,bound.calls); TEST_ASSERT_EQUAL_UINT(2,x.memory.writes);
}
void test_invalid_inputs_and_rng_failure_do_not_commit() {
    Fixture x; status(t::Status::OutputTooSmall,x.join(nullptr,34));
    x.random.fail = true; status(t::Status::StorageError,x.join()); x.random.fail = false;
    x.wire[32] ^= 1; status(t::Status::InvalidFrame,x.join()); x.seal();
    uint8_t uid[10]{};
    status(t::Status::WrongUid,x.transaction.request(factoryMac,uid,up,x.wire,33,7,123,down,x.accept,35));
    status(t::Status::InvalidAssignment,x.transaction.request(factoryMac,x.request.identity.uid,up,x.wire,33,0,123,down,x.accept,35));
    TEST_ASSERT_EQUAL_UINT(0,x.memory.writes);
    status(t::Status::Accept,x.join()); x.confirmWire(); Bound bound; uint8_t output[25];
    status(t::Status::OutputTooSmall,x.transaction.confirm(confirmUp,x.proof,27,bound,output,24));
    x.proof[26] ^= 1;
    status(t::Status::InvalidFrame,x.transaction.confirm(confirmUp,x.proof,27,bound,output,25));
    TEST_ASSERT_EQUAL_UINT(0,bound.calls); x.confirmWire(); bound.fail = true;
    status(t::Status::StorageError,x.transaction.confirm(confirmUp,x.proof,27,bound,output,25));
    TEST_ASSERT_EQUAL_UINT(1,x.memory.writes);
}
void test_snapshot_every_byte_corruption_fails_closed_until_explicit_clear() {
    Fixture x; status(t::Status::Accept,x.join());
    uint8_t pristine[t::kSnapshotSize]; memcpy(pristine,x.memory.bytes,sizeof(pristine));
    for (size_t i = 0; i < sizeof(pristine); ++i) {
        memcpy(x.memory.bytes,pristine,sizeof(pristine)); x.memory.bytes[i] ^= 1;
        t::Transaction reboot(x.memory,x.random); TEST_ASSERT_FALSE(reboot.load());
        status(t::Status::StorageError,reboot.request(factoryMac,x.request.identity.uid,up,x.wire,33,7,123,down,x.accept,35));
    }
    TEST_ASSERT_FALSE(x.transaction.load()); TEST_ASSERT_TRUE(x.transaction.clear());
    status(t::Status::Accept,x.join());
}
void test_atomic_power_loss_restores_old_or_new_transaction() {
    Fixture x; status(t::Status::Accept,x.join());
    Memory old = x.memory;
    ++x.request.identity.requestNonce; x.seal(); status(t::Status::Accept,x.join());
    Memory newer = x.memory;
    // The backend contract is an atomic committed blob; interruption may retain
    // either whole version. Every partial blob is covered by corruption tests.
    for (unsigned version = 0; version < 2; ++version) {
        Memory storage = version ? newer : old;
        t::Transaction reboot(storage,x.random); TEST_ASSERT_TRUE(reboot.load());
        TEST_ASSERT_EQUAL_UINT32(version ? 2 : 1,reboot.generation());
        TEST_ASSERT_EQUAL_UINT32(version ? 2 : 1,p::readUint32Le(reboot.record().request+11));
        uint8_t out[35];
        status(t::Status::RepeatedAccept,reboot.request(factoryMac,x.request.identity.uid,up,reboot.record().request,33,
                                                      7,123,down,out,35));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(reboot.record().accept,out,35);
    }
}
void test_node_and_gateway_complete_the_same_persisted_transaction() {
    struct Eeprom {
        uint8_t data[64];
        Eeprom() { memset(data,0xFF,sizeof(data)); }
        uint8_t read(size_t i) { return data[i]; }
        void update(size_t i, uint8_t value) { data[i] = value; }
    } eeprom;
    Fixture gateway;
    radiosensors::node::security_pairing::Transaction<Eeprom> node(eeprom,gateway.request.identity.uid);
    TEST_ASSERT_FALSE(node.load()); TEST_ASSERT_TRUE(node.begin(gateway.request.identity.requestNonce));
    status(t::Status::Accept,gateway.join());
    TEST_ASSERT_EQUAL_UINT8(0,static_cast<uint8_t>(node.accept(factoryMac,down,gateway.accept,35)));
    s::Context context; s::initialize(context,factory,node.config().salt);
    uint8_t confirm[27], complete[25]; Bound bound;
    TEST_ASSERT_TRUE(node.confirm(context.authentication,confirm,27));
    status(t::Status::Complete,gateway.transaction.confirm(confirmUp,confirm,27,bound,complete,25));
    const auto completed = node.complete(context.authentication,s::Transport{7,100,0},complete,25);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(radiosensors::node::security_pairing::Status::Active),
                           static_cast<uint8_t>(completed));
    radiosensors::node::security_pairing::Transaction<Eeprom> reboot(eeprom,gateway.request.identity.uid);
    TEST_ASSERT_TRUE(reboot.load()); TEST_ASSERT_EQUAL_UINT8(2,static_cast<uint8_t>(reboot.config().state));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(gateway.transaction.record().accept+19,reboot.config().salt,8);
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accept_is_durable_immutable_and_keys_match_node);
    RUN_TEST(test_new_attempt_retires_old_counter_and_conflicting_nonce);
    RUN_TEST(test_confirm_is_durable_and_repeat_never_initializes_bound);
    RUN_TEST(test_failed_activation_commit_retries_bound_idempotently);
    RUN_TEST(test_no_output_without_successful_write_and_exact_readback);
    RUN_TEST(test_activation_committed_without_readback_recovers_as_active);
    RUN_TEST(test_invalid_inputs_and_rng_failure_do_not_commit);
    RUN_TEST(test_snapshot_every_byte_corruption_fails_closed_until_explicit_clear);
    RUN_TEST(test_atomic_power_loss_restores_old_or_new_transaction);
    RUN_TEST(test_node_and_gateway_complete_the_same_persisted_transaction);
    return UNITY_END();
}
