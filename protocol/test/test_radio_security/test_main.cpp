#include <RadioSecurity.h>
#include <unity.h>
#include <string.h>
#include <initializer_list>
#include <fstream>
#include <string>

using namespace radiosensors::security;
namespace {
Context context;
const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
const uint8_t salt[8] = {16,17,18,19,20,21,22,23};
const uint8_t data[48] = {2,0xBA,0xE4,0x0C,0x2E,9,0xD7,0x11,0x94,0x27};
constexpr Transport uplink{100,7,0x40};
constexpr Transport downlink{7,100,0x80};
constexpr uint32_t counter = 0x12345678;
bool openFrame(uint8_t* wire, size_t size, Transport transport = uplink) {
    uint32_t decodedCounter = 0;
    uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    return open(context, Direction::Node, 7, transport, wire, size,
                decodedCounter, payload, payloadSize);
}
uint8_t digit(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
void unhex(const char* text, uint8_t* output) {
    while (*text) { *output++ = static_cast<uint8_t>((digit(text[0]) << 4) | digit(text[1])); text += 2; }
}
void assertHex(const char* expected, const uint8_t* actual) {
    uint8_t bytes[64];
    unhex(expected, bytes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, actual, strlen(expected) / 2);
}
std::string vectorHex(const char* name) {
    std::ifstream file("protocol-vectors.json");
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t section = json.find("\"crypto_v3\"");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, section);
    const size_t field = json.find(std::string("\"") + name + "\"", section);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, field);
    const size_t start = json.find('"', json.find(':', field) + 1) + 1;
    return json.substr(start, json.find('"', start) - start);
}
}
void setUp() { initialize(context, factory, salt); }
void tearDown() {}

void test_standard_known_answers() {
    // FIPS-197 C.1; SP 800-38A F.5.1; RFC 4493 section 4.
    osk::crypto::Aes128 aes;
    uint8_t key[16], block[16], nonce[16];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    osk::crypto::aesExpandKey(aes, key);
    unhex("00112233445566778899aabbccddeeff", block);
    osk::crypto::aesEncrypt(aes, block);
    assertHex("69c4e0d86a7b0430d8cdb78070b4c55a", block);
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    osk::crypto::aesExpandKey(aes, key);
    unhex("6bc1bee22e409f96e93d7e117393172a", block);
    unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", nonce);
    osk::crypto::ctrApply(aes, nonce, block, 16);
    assertHex("874d6191b620e3261bef6864990db6ce", block);
    osk::crypto::Cmac mac;
    osk::crypto::cmacInit(mac, key);
    assertHex("fbeed618357133667c85e08f7236a8de", mac.k1);
    assertHex("f7ddac306ae266ccf90bc11ee46d513b", mac.k2);
    uint8_t message[64];
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", message);
    const size_t lengths[] = {0,16,40,64};
    const char* tags[] = {"bb1d6929e95937287fa37d129b756746", "070a16b46b4d4144f79bdd9dd04a287c", "dfa66747de9ae63030ca32611497c827", "51f0bebf7e3b9d92fc49741779363cfe"};
    for (size_t i = 0; i < 4; ++i) {
        osk::crypto::cmacCompute(mac, message, lengths[i], block);
        assertHex(tags[i], block);
    }
}
void test_counter_block_format() {
    uint8_t nonce[16];
    counterBlock(Direction::Gateway, 7, 0x64, counter, 2, nonce);
    const uint8_t expected[16] = {1,7,0x64,0x78,0x56,0x34,0x12,0,0,0,0,0,0,0,0,2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, nonce, 16);
}
void test_independent_v3_known_answers() {
    Keys keys;
    osk::crypto::Cmac scratch;
    deriveKeys(factory, salt, keys, scratch);
    assertHex(vectorHex("encryption_key").c_str(), keys.encryption);
    assertHex(vectorHex("authentication_key").c_str(), keys.authentication);
    uint8_t wire[61];
    size_t size = 0;
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, wire, sizeof(wire), size));
    assertHex(vectorHex("telemetry").c_str(), wire);
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, kActivationHeader, data, 10, wire, sizeof(wire), size, salt));
    assertHex(vectorHex("activation").c_str(), wire);
    TEST_ASSERT_TRUE(seal(context, Direction::Gateway, 7, Transport{7,100,0}, counter, 0x64, data, 10, wire, sizeof(wire), size));
    assertHex(vectorHex("command").c_str(), wire);
    Ack ack;
    ack.commandPending = true;
    ack.hasPowerTarget = true;
    ack.powerTarget = 23;
    ack.hasCounterFloor = true;
    ack.counterFloor = counter + 256;
    TEST_ASSERT_TRUE(sealAck(context.authentication, downlink, counter, ack, wire, sizeof(wire), size));
    assertHex(vectorHex("ack_floor").c_str(), wire);
    const uint8_t confirm[] = {0x63,1,2,3,4,5,6,7,8,9,10,0x78,0x76,0x54,0x12,0,1,2,3};
    TEST_ASSERT_TRUE(sealJoin(context.authentication, Transport{100,7,0}, confirm, sizeof(confirm), kNodeTagSize, wire, sizeof(wire), size, salt));
    assertHex(vectorHex("join_confirm").c_str(), wire);
}
void test_roundtrip_and_every_bit_authenticated() {
    for (Direction direction : {Direction::Node, Direction::Gateway}) {
        const Transport transport = direction == Direction::Node ? uplink : downlink;
        for (size_t length : {size_t(0), size_t(4), size_t(8), size_t(9), size_t(10), size_t(16), size_t(24), size_t(48)}) {
            uint8_t frame[61], copy[61];
            size_t size = 0;
            TEST_ASSERT_TRUE(seal(context, direction, 7, transport, counter, 0x60, data, length, frame, sizeof(frame), size));
            for (size_t i = 0; i < size; ++i) {
                for (uint8_t bit = 1; bit != 0; bit <<= 1) {
                    memcpy(copy, frame, size);
                    copy[i] ^= bit;
                    uint8_t before[61];
                    memcpy(before, copy, size);
                    uint32_t decodedCounter = 0;
                    uint8_t* payload = nullptr;
                    size_t payloadSize = 0;
                    TEST_ASSERT_FALSE(open(context, direction, 7, transport, copy, size, decodedCounter, payload, payloadSize));
                    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, copy, size);
                }
            }
            uint32_t decodedCounter = 0;
            uint8_t* payload = nullptr;
            size_t payloadSize = 0;
            TEST_ASSERT_TRUE(open(context, direction, 7, transport, frame, size, decodedCounter, payload, payloadSize));
            TEST_ASSERT_EQUAL_HEX32(counter, decodedCounter);
            TEST_ASSERT_EQUAL(length, payloadSize);
            if (length != 0) TEST_ASSERT_EQUAL_HEX8_ARRAY(data, payload, length);
        }
    }
}
void test_transport_authenticated() {
    uint8_t frame[61];
    size_t size = 0;
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, frame, sizeof(frame), size));
    for (Transport changed : {Transport{101,7,0x40}, Transport{100,8,0x40}, Transport{100,7,0}}) {
        TEST_ASSERT_FALSE(openFrame(frame, size, changed));
    }
    uint32_t decodedCounter = 0;
    uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    TEST_ASSERT_FALSE(open(context, Direction::Node, 8, uplink, frame, size, decodedCounter, payload, payloadSize));
}
void test_retransmission_and_nonce_separation() {
    uint8_t frame[61], other[61];
    size_t size = 0, otherSize = 0;
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, frame, sizeof(frame), size));
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, other, sizeof(other), otherSize));
    TEST_ASSERT_EQUAL(size, otherSize);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame, other, size);
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter + 1, 0x60, data, 10, other, sizeof(other), otherSize));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(frame + 5, other + 5, 10));
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x65, data, 10, other, sizeof(other), otherSize));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(frame + 5, other + 5, 10));
    TEST_ASSERT_TRUE(seal(context, Direction::Gateway, 7, downlink, counter, 0x60, data, 10, other, sizeof(other), otherSize));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(frame + 5, other + 5, 10));
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 8, Transport{100,8,0x40}, counter, 0x60, data, 10, other, sizeof(other), otherSize));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(frame + 5, other + 5, 10));
}
void test_activation_clear_challenge_authenticated() {
    uint8_t frame[61], copy[61];
    size_t size = 0;
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, kActivationHeader, data, 10, frame, sizeof(frame), size, salt));
    TEST_ASSERT_EQUAL(31, size);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(salt, frame + 5, 8);
    memcpy(copy, frame, size);
    copy[5] ^= 1;
    TEST_ASSERT_FALSE(openFrame(copy, size));
    TEST_ASSERT_TRUE(openFrame(frame, size));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, frame + 13, 10);
}
void test_ack_options_counter_and_transport_binding() {
    for (uint8_t flags = 0; flags < 16; ++flags) {
        Ack ack{};
        ack.commandPending = (flags & 1) != 0;
        ack.hasPowerTarget = (flags & 2) != 0;
        ack.powerTarget = 23;
        ack.hasCounterFloor = (flags & 4) != 0;
        ack.counterFloor = counter + 256;
        ack.hasChallenge = (flags & 8) != 0;
        memcpy(ack.challenge, salt, 8);
        uint8_t wire[16];
        size_t size = 0;
        if ((flags & 12) == 12) {
            TEST_ASSERT_FALSE(sealAck(context.authentication, downlink, counter, ack, wire, sizeof(wire), size));
            continue;
        }
        TEST_ASSERT_TRUE(sealAck(context.authentication, downlink, counter, ack, wire, sizeof(wire), size));
        Ack decoded{};
        TEST_ASSERT_FALSE(openAck(context.authentication, downlink, counter + 1, wire, size, decoded));
        TEST_ASSERT_FALSE(openAck(context.authentication, Transport{7,100,0}, counter, wire, size, decoded));
        TEST_ASSERT_TRUE(openAck(context.authentication, downlink, counter, wire, size, decoded));
        TEST_ASSERT_EQUAL(ack.commandPending, decoded.commandPending);
        TEST_ASSERT_EQUAL(ack.hasPowerTarget, decoded.hasPowerTarget);
        TEST_ASSERT_EQUAL(ack.hasCounterFloor, decoded.hasCounterFloor);
        TEST_ASSERT_EQUAL(ack.hasChallenge, decoded.hasChallenge);
        if (ack.hasPowerTarget) TEST_ASSERT_EQUAL(23, decoded.powerTarget);
        if (ack.hasCounterFloor) TEST_ASSERT_EQUAL_HEX32(ack.counterFloor, decoded.counterFloor);
        if (ack.hasChallenge) TEST_ASSERT_EQUAL_HEX8_ARRAY(salt, decoded.challenge, 8);
        for (size_t i = 0; i < size; ++i) {
            wire[i] ^= 1;
            TEST_ASSERT_FALSE(openAck(context.authentication, downlink, counter, wire, size, decoded));
            wire[i] ^= 1;
        }
    }
}
void test_join_cleartext_and_implicit_salt_binding() {
    uint8_t request[25] = {0x61,1,2,3,4,5,6,7,8,9,10};
    uint8_t wire[61];
    size_t size = 0;
    TEST_ASSERT_TRUE(sealJoin(context.authentication, uplink, request, sizeof(request), kNodeTagSize, wire, sizeof(wire), size));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(request, wire, sizeof(request));
    TEST_ASSERT_TRUE(verifyJoin(context.authentication, uplink, wire, size, kNodeTagSize));
    TEST_ASSERT_FALSE(verifyJoin(context.authentication, downlink, wire, size, kNodeTagSize));
    request[0] = 0x63;
    TEST_ASSERT_TRUE(sealJoin(context.authentication, uplink, request, sizeof(request), kNodeTagSize, wire, sizeof(wire), size, salt));
    TEST_ASSERT_TRUE(verifyJoin(context.authentication, uplink, wire, size, kNodeTagSize, salt));
    uint8_t wrongSalt[8]{};
    TEST_ASSERT_FALSE(verifyJoin(context.authentication, uplink, wire, size, kNodeTagSize, wrongSalt));
    TEST_ASSERT_FALSE(verifyJoin(context.authentication, uplink, wire, size, kNodeTagSize));
}
void test_input_bounds() {
    uint8_t wire[61];
    size_t size = 99;
    TEST_ASSERT_FALSE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 49, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, wire, 22, size));
    TEST_ASSERT_FALSE(seal(context, Direction::Node, 7, uplink, counter, 0x61, data, 10, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(seal(context, Direction::Node, 7, uplink, counter, kActivationHeader, data, 10, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(seal(context, Direction::Gateway, 7, downlink, counter, kActivationHeader, data, 10, wire, sizeof(wire), size, salt));
    TEST_ASSERT_FALSE(sealJoin(context.authentication, uplink, data, 0, 8, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(verifyJoin(context.authentication, uplink, wire, 61, 0));
}
void test_repairing_changes_keys_and_rejects_old_frames() {
    uint8_t wire[61], newSalt[8];
    size_t size = 0;
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, wire, sizeof(wire), size));
    memcpy(newSalt, salt, sizeof(salt));
    newSalt[0] ^= 1;
    initialize(context, factory, newSalt);
    TEST_ASSERT_FALSE(openFrame(wire, size));
}
void test_in_place_payload_sealing() {
    uint8_t wire[61], separate[61];
    size_t size = 0, separateSize = 0;
    memcpy(wire, data, 10);
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, wire, 10, wire, sizeof(wire), size));
    TEST_ASSERT_TRUE(seal(context, Direction::Node, 7, uplink, counter, 0x60, data, 10, separate, sizeof(separate), separateSize));
    TEST_ASSERT_EQUAL(size, separateSize);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(separate, wire, size);
}
void test_authenticated_ack_rejects_reserved_flags_and_bad_lengths() {
    for (uint8_t flags : {uint8_t(0x10), uint8_t(0x0c), uint8_t(0x02), uint8_t(0x04), uint8_t(0x08)}) {
        uint8_t input[] = {kAckHeader,7,100,0x80,0x78,0x56,0x34,0x12,flags};
        uint8_t tag[16], wire[7];
        osk::crypto::cmacCompute(context.authentication, input, sizeof(input), tag);
        wire[0] = flags;
        memcpy(wire + 1, tag, 6);
        Ack ack;
        TEST_ASSERT_FALSE(openAck(context.authentication, downlink, counter, wire, sizeof(wire), ack));
    }
    uint8_t input[] = {kAckHeader,7,100,0x80,0x78,0x56,0x34,0x12};
    uint8_t tag[16];
    osk::crypto::cmacCompute(context.authentication, input, sizeof(input), tag);
    Ack empty;
    TEST_ASSERT_TRUE(openAck(context.authentication, downlink, counter, tag, 6, empty));
    TEST_ASSERT_FALSE(empty.commandPending);
    TEST_ASSERT_FALSE(empty.hasPowerTarget);
    TEST_ASSERT_FALSE(empty.hasCounterFloor);
    TEST_ASSERT_FALSE(empty.hasChallenge);
}
int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_standard_known_answers);
    RUN_TEST(test_counter_block_format);
    RUN_TEST(test_independent_v3_known_answers);
    RUN_TEST(test_roundtrip_and_every_bit_authenticated);
    RUN_TEST(test_transport_authenticated);
    RUN_TEST(test_retransmission_and_nonce_separation);
    RUN_TEST(test_activation_clear_challenge_authenticated);
    RUN_TEST(test_ack_options_counter_and_transport_binding);
    RUN_TEST(test_join_cleartext_and_implicit_salt_binding);
    RUN_TEST(test_input_bounds);
    RUN_TEST(test_repairing_changes_keys_and_rejects_old_frames);
    RUN_TEST(test_in_place_payload_sealing);
    RUN_TEST(test_authenticated_ack_rejects_reserved_flags_and_bad_lengths);
    return UNITY_END();
}
