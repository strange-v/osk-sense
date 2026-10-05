#include <RadioSecurityPairing.h>
#include <unity.h>
#include <string.h>

namespace s = radiosensors::security;
namespace f = s::frames;
namespace n = radiosensors::node;
namespace sp = n::security_pairing;
namespace ss = n::security_storage;
namespace {
const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
const uint8_t uid[10] = {0,1,2,3,4,5,6,7,8,9};
osk::crypto::Cmac factoryMac;
struct Memory {
    uint8_t bytes[128];
    int remaining = -1;
    unsigned writes = 0;
    Memory() { memset(bytes,0xFF,sizeof(bytes)); }
    uint8_t read(size_t address) const { return bytes[address]; }
    void update(size_t address, uint8_t value) {
        ++writes;
        if (remaining == 0) return;
        if (remaining > 0) --remaining;
        bytes[address] = value;
    }
};
struct Fixture {
    Memory memory;
    sp::Transaction<Memory> transaction{memory,uid};
    f::JoinAccept accept{};
    uint8_t wire[35]{};
    Fixture() {
        TEST_ASSERT_FALSE(transaction.load()); TEST_ASSERT_TRUE(transaction.begin(0x1234567800000400ULL));
        accept.identity = transaction.identity(); accept.nodeId = 7; accept.networkId = 123;
        for (uint8_t i = 0; i < 8; ++i) accept.salt[i] = i+16;
        seal();
    }
    void seal() { TEST_ASSERT_TRUE(f::sealJoinAccept(factoryMac,s::Transport{0,100,0},accept,wire,sizeof(wire))); }
    sp::Status receive() { return transaction.accept(factoryMac,s::Transport{0,100,0},wire,sizeof(wire)); }
    void mac(osk::crypto::Cmac& out) {
        s::Keys keys; s::deriveKeys(factory,accept.salt,keys,out);
        osk::crypto::cmacInit(out,keys.authentication);
    }
};
void status(sp::Status expected, sp::Status value) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),static_cast<uint8_t>(value));
}
}
void setUp() { osk::crypto::cmacInit(factoryMac,factory); }
void tearDown() {}
void test_first_accept_is_pinned_across_reboot() {
    Fixture x; status(sp::Status::Accepted,x.receive());
    unsigned writes = x.memory.writes;
    ++x.accept.salt[0]; x.seal(); status(sp::Status::Pinned,x.receive());
    TEST_ASSERT_EQUAL_UINT(writes,x.memory.writes);
    TEST_ASSERT_EQUAL_UINT8(16,x.transaction.config().salt[0]);
    sp::Transaction<Memory> reboot(x.memory,uid); TEST_ASSERT_TRUE(reboot.load());
    TEST_ASSERT_FALSE(reboot.begin(42));
    status(sp::Status::Pinned,reboot.accept(factoryMac,s::Transport{0,100,0},x.wire,35));
    TEST_ASSERT_EQUAL_UINT8(16,reboot.config().salt[0]);
    osk::crypto::Cmac mac; --x.accept.salt[0]; x.mac(mac);
    uint8_t confirm[27]; TEST_ASSERT_TRUE(reboot.confirm(mac,confirm,sizeof(confirm)));
    f::JoinIdentity proof;
    TEST_ASSERT_TRUE(f::openJoinProof(mac,s::Transport{100,7,0},confirm,27,x.accept.salt,false,proof));
    TEST_ASSERT_EQUAL_HEX64(x.accept.identity.requestNonce,proof.requestNonce);
}
void test_wrong_uid_nonce_tag_and_transport_never_write() {
    Fixture x; uint8_t confirm[27]; osk::crypto::Cmac mac; x.mac(mac);
    TEST_ASSERT_FALSE(x.transaction.confirm(mac,confirm,27));
    ++x.accept.identity.requestNonce; x.seal(); status(sp::Status::InvalidFrame,x.receive());
    --x.accept.identity.requestNonce; ++x.accept.identity.uid[0]; x.seal(); status(sp::Status::InvalidFrame,x.receive());
    --x.accept.identity.uid[0]; x.seal(); x.wire[34] ^= 1; status(sp::Status::InvalidFrame,x.receive());
    x.seal(); status(sp::Status::InvalidFrame,x.transaction.accept(factoryMac,s::Transport{7,100,0},x.wire,35));
    TEST_ASSERT_EQUAL_UINT(0,x.memory.writes);
}
void test_every_accept_write_interruption_blocks_confirm_until_reload() {
    Fixture baseline; status(sp::Status::Accepted,baseline.receive());
    const unsigned updates = baseline.memory.writes;
    for (unsigned cut = 0; cut <= updates; ++cut) {
        Fixture x; x.memory.remaining = static_cast<int>(cut);
        const auto result = x.receive(); osk::crypto::Cmac mac; x.mac(mac); uint8_t confirm[27];
        if (result == sp::Status::StorageError) {
            TEST_ASSERT_FALSE(x.transaction.confirm(mac,confirm,27));
            status(sp::Status::StorageError,x.receive()); TEST_ASSERT_FALSE(x.transaction.begin(9999));
        }
        x.memory.remaining = -1;
        sp::Transaction<Memory> reboot(x.memory,uid);
        if (reboot.load()) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(x.accept.salt,reboot.config().salt,8);
            TEST_ASSERT_TRUE(reboot.confirm(mac,confirm,27));
        } else TEST_ASSERT_FALSE(reboot.confirm(mac,confirm,27));
    }
}
void test_complete_uses_pinned_salt_and_is_idempotent() {
    Fixture x; status(sp::Status::Accepted,x.receive());
    osk::crypto::Cmac mac; x.mac(mac); uint8_t complete[25];
    uint8_t wrong[8]; memcpy(wrong,x.accept.salt,8); ++wrong[0];
    TEST_ASSERT_TRUE(f::sealJoinProof(mac,s::Transport{7,100,0},x.accept.identity,wrong,true,complete,25));
    status(sp::Status::InvalidFrame,x.transaction.complete(mac,s::Transport{7,100,0},complete,25));
    TEST_ASSERT_TRUE(f::sealJoinProof(mac,s::Transport{7,100,0},x.accept.identity,x.accept.salt,true,complete,25));
    status(sp::Status::Active,x.transaction.complete(mac,s::Transport{7,100,0},complete,25));
    unsigned writes = x.memory.writes;
    status(sp::Status::Active,x.transaction.complete(mac,s::Transport{7,100,0},complete,25));
    TEST_ASSERT_EQUAL_UINT(writes,x.memory.writes);
    sp::Transaction<Memory> reboot(x.memory,uid); TEST_ASSERT_TRUE(reboot.load());
    TEST_ASSERT_EQUAL_UINT8(2,static_cast<uint8_t>(reboot.config().state));
    uint8_t confirm[27]; TEST_ASSERT_FALSE(reboot.confirm(mac,confirm,27));
}
void test_every_complete_write_interruption_preserves_pinned_keys() {
    Fixture baseline; status(sp::Status::Accepted,baseline.receive());
    osk::crypto::Cmac mac; baseline.mac(mac); uint8_t complete[25];
    TEST_ASSERT_TRUE(f::sealJoinProof(mac,s::Transport{7,100,0},baseline.accept.identity,baseline.accept.salt,true,complete,25));
    unsigned before = baseline.memory.writes;
    status(sp::Status::Active,baseline.transaction.complete(mac,s::Transport{7,100,0},complete,25));
    unsigned updates = baseline.memory.writes - before;
    for (unsigned cut = 0; cut <= updates; ++cut) {
        Fixture x; status(sp::Status::Accepted,x.receive()); x.memory.remaining = static_cast<int>(cut);
        x.transaction.complete(mac,s::Transport{7,100,0},complete,25);
        x.memory.remaining = -1; sp::Transaction<Memory> reboot(x.memory,uid); TEST_ASSERT_TRUE(reboot.load());
        TEST_ASSERT_EQUAL_HEX8_ARRAY(x.accept.salt,reboot.config().salt,8);
        TEST_ASSERT_EQUAL_HEX64(x.accept.identity.requestNonce,reboot.config().requestNonce);
        status(sp::Status::Active,reboot.complete(mac,s::Transport{7,100,0},complete,25));
    }
}
int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_accept_is_pinned_across_reboot);
    RUN_TEST(test_wrong_uid_nonce_tag_and_transport_never_write);
    RUN_TEST(test_every_accept_write_interruption_blocks_confirm_until_reload);
    RUN_TEST(test_complete_uses_pinned_salt_and_is_idempotent);
    RUN_TEST(test_every_complete_write_interruption_preserves_pinned_keys);
    return UNITY_END();
}
