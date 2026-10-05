#include <RadioSecurityFrames.h>
#include <GatewayReplay.h>
#include <unity.h>
#include <string.h>
#include <fstream>
#include <string>
#include <initializer_list>

namespace s = radiosensors::security;
namespace f = radiosensors::security::frames;
namespace r = radiosensors::replay;
namespace p = radiosensors::protocol;
namespace {
const uint8_t factory[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
const uint8_t salt[8] = {16,17,18,19,20,21,22,23};
constexpr s::Transport joinUp{100,0,0}, joinDown{0,100,0};
constexpr s::Transport up{100,7,0x40}, down{7,100,0};
constexpr uint32_t counter = 0x12345678;
osk::crypto::Cmac factoryMac;
s::Context context;
f::JoinIdentity identity() {
    f::JoinIdentity value;
    for (uint8_t i = 0; i < 10; ++i) value.uid[i] = i;
    value.requestNonce = 0x0403020112345678ULL;
    return value;
}
void vector(const char* name, const uint8_t* wire, size_t size) {
    std::ifstream file("protocol-vectors.json");
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t field = json.find(std::string("\"") + name + "\"");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, field);
    const size_t start = json.find('"', json.find(':', field) + 1) + 1;
    const std::string hex = json.substr(start, json.find('"', start) - start);
    TEST_ASSERT_EQUAL_UINT(size * 2, hex.size());
    for (size_t i = 0; i < size; ++i)
        TEST_ASSERT_EQUAL_HEX8(std::stoul(hex.substr(i * 2, 2), nullptr, 16), wire[i]);
}
struct Memory : r::BlobStorage {
    uint8_t blob[r::kSnapshotSize]{};
    size_t stored = 0;
    r::ReadStatus read(uint8_t* out, size_t capacity, size_t& size) override {
        size = stored;
        if (!stored) return r::ReadStatus::Missing;
        if (capacity < stored) return r::ReadStatus::Error;
        memcpy(out, blob, stored);
        return r::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        memcpy(blob, data, size);
        stored = size;
        return true;
    }
};
struct Random : r::RandomSource {
    bool fill(uint8_t*, size_t) override { return false; }
};
void action(r::Action expected, r::Decision decision) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected), static_cast<uint8_t>(decision.action));
}
}
void setUp() { osk::crypto::cmacInit(factoryMac, factory); s::initialize(context, factory, salt); }
void tearDown() {}

void test_join_known_answers_and_full_nonce() {
    uint8_t wire[61];
    f::JoinRequest request;
    request.identity = identity(); request.profileId = 6; request.firmware = {1,2,3}; request.maxPowerLevel = 31;
    TEST_ASSERT_TRUE(f::sealJoinRequest(factoryMac, joinUp, request, wire, sizeof(wire)));
    vector("pairing_request", wire, f::kJoinRequestSize);
    f::JoinRequest decoded;
    TEST_ASSERT_TRUE(f::openJoinRequest(factoryMac, joinUp, wire, f::kJoinRequestSize, decoded));
    TEST_ASSERT_EQUAL_HEX64(request.identity.requestNonce, decoded.identity.requestNonce);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(request.identity.uid, decoded.identity.uid, 10);
    TEST_ASSERT_EQUAL_UINT16(6, decoded.profileId);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.firmware.patch);
    TEST_ASSERT_EQUAL_UINT8(31, decoded.maxPowerLevel);
    f::JoinAccept accept;
    accept.identity = identity(); memcpy(accept.salt, salt, 8); accept.nodeId = 7; accept.networkId = 123;
    TEST_ASSERT_TRUE(f::sealJoinAccept(factoryMac, joinDown, accept, wire, sizeof(wire)));
    vector("pairing_accept", wire, f::kJoinAcceptSize);
    f::JoinAccept accepted;
    TEST_ASSERT_TRUE(f::openJoinAccept(factoryMac, joinDown, wire, f::kJoinAcceptSize, accepted));
    TEST_ASSERT_EQUAL_HEX64(accept.identity.requestNonce, accepted.identity.requestNonce);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(salt, accepted.salt, 8);
    TEST_ASSERT_EQUAL_UINT8(7, accepted.nodeId);
    TEST_ASSERT_EQUAL_UINT8(123, accepted.networkId);
    for (bool complete : {false, true}) {
        const s::Transport transport = complete ? joinDown : joinUp;
        const size_t size = complete ? f::kJoinCompleteSize : f::kJoinConfirmSize;
        TEST_ASSERT_TRUE(f::sealJoinProof(context.authentication, transport, identity(), salt, complete, wire, sizeof(wire)));
        vector(complete ? "pairing_complete" : "pairing_confirm", wire, size);
        f::JoinIdentity proof;
        TEST_ASSERT_TRUE(f::openJoinProof(context.authentication, transport, wire, size, salt, complete, proof));
        TEST_ASSERT_EQUAL_HEX64(identity().requestNonce, proof.requestNonce);
    }
}

void test_pairing_every_bit_transport_and_salt_authenticated_before_output() {
    uint8_t wire[61], changed[61];
    f::JoinRequest request;
    request.identity = identity(); request.profileId = 6;
    TEST_ASSERT_TRUE(f::sealJoinRequest(factoryMac, joinUp, request, wire, sizeof(wire)));
    f::JoinRequest output; output.profileId = 999;
    for (size_t i = 0; i < f::kJoinRequestSize; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
        memcpy(changed, wire, sizeof(wire)); changed[i] ^= 1U << bit;
        TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, joinUp, changed, f::kJoinRequestSize, output));
        TEST_ASSERT_EQUAL_UINT16(999, output.profileId);
    }
    TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, s::Transport{101,0,0}, wire, f::kJoinRequestSize, output));
    TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, s::Transport{100,1,0}, wire, f::kJoinRequestSize, output));
    TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, s::Transport{100,0,0x40}, wire, f::kJoinRequestSize, output));
    f::JoinAccept accept;
    accept.identity = identity(); accept.nodeId = 7; accept.networkId = 123;
    TEST_ASSERT_TRUE(f::sealJoinAccept(factoryMac, joinDown, accept, wire, sizeof(wire)));
    f::JoinAccept accepted; accepted.nodeId = 99;
    for (size_t i = 0; i < f::kJoinAcceptSize; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
        memcpy(changed, wire, sizeof(wire)); changed[i] ^= 1U << bit;
        TEST_ASSERT_FALSE(f::openJoinAccept(factoryMac, joinDown, changed, f::kJoinAcceptSize, accepted));
        TEST_ASSERT_EQUAL_UINT8(99, accepted.nodeId);
    }
    for (bool complete : {false, true}) {
        const auto transport = complete ? joinDown : joinUp;
        const size_t size = complete ? f::kJoinCompleteSize : f::kJoinConfirmSize;
        TEST_ASSERT_TRUE(f::sealJoinProof(context.authentication, transport, identity(), salt, complete, wire, sizeof(wire)));
        f::JoinIdentity proof; proof.requestNonce = 999;
        for (size_t i = 0; i < size; ++i) for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(changed, wire, sizeof(wire)); changed[i] ^= 1U << bit;
            TEST_ASSERT_FALSE(f::openJoinProof(context.authentication, transport, changed, size, salt, complete, proof));
            TEST_ASSERT_EQUAL_UINT64(999, proof.requestNonce);
        }
        uint8_t wrongSalt[8]; memcpy(wrongSalt, salt, 8); wrongSalt[0] ^= 1;
        TEST_ASSERT_FALSE(f::openJoinProof(context.authentication, transport, wire, size, wrongSalt, complete, proof));
        TEST_ASSERT_FALSE(f::openJoinProof(context.authentication, transport, wire, size, salt, !complete, proof));
    }
}

void test_join_bounds_and_authenticated_invalid_fields() {
    uint8_t wire[61], raw[29]{};
    f::JoinRequest request; request.profileId = 6;
    f::JoinRequest decoded; decoded.profileId = 999;
    TEST_ASSERT_FALSE(f::sealJoinRequest(factoryMac, joinUp, request, wire, f::kJoinRequestSize - 1));
    request.profileId = 0;
    TEST_ASSERT_FALSE(f::sealJoinRequest(factoryMac, joinUp, request, wire, sizeof(wire)));
    request.profileId = 6; request.maxPowerLevel = 32;
    TEST_ASSERT_FALSE(f::sealJoinRequest(factoryMac, joinUp, request, wire, sizeof(wire)));
    raw[0] = f::kJoinRequestHeader;
    size_t size = 0;
    TEST_ASSERT_TRUE(s::sealJoin(factoryMac, joinUp, raw, 25, 8, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, joinUp, wire, size, decoded));
    raw[19] = 6; raw[24] = 32;
    TEST_ASSERT_TRUE(s::sealJoin(factoryMac, joinUp, raw, 25, 8, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(f::openJoinRequest(factoryMac, joinUp, wire, size, decoded));
    TEST_ASSERT_EQUAL_UINT16(999, decoded.profileId);
    f::JoinAccept accept; accept.nodeId = 7; accept.networkId = 123;
    TEST_ASSERT_FALSE(f::sealJoinAccept(factoryMac, joinDown, accept, wire, f::kJoinAcceptSize - 1));
    for (uint8_t invalid : {uint8_t(0),uint8_t(100),uint8_t(255)}) {
        accept.nodeId = invalid;
        TEST_ASSERT_FALSE(f::sealJoinAccept(factoryMac, joinDown, accept, wire, sizeof(wire)));
        raw[0] = f::kJoinAcceptHeader; raw[27] = invalid; raw[28] = 123;
        TEST_ASSERT_TRUE(s::sealJoin(factoryMac, joinDown, raw, 29, 6, wire, sizeof(wire), size));
        TEST_ASSERT_FALSE(f::openJoinAccept(factoryMac, joinDown, wire, size, accept));
    }
    accept.nodeId = 7; accept.networkId = 0;
    TEST_ASSERT_FALSE(f::sealJoinAccept(factoryMac, joinDown, accept, wire, sizeof(wire)));
    raw[27] = 7; raw[28] = 0;
    TEST_ASSERT_TRUE(s::sealJoin(factoryMac, joinDown, raw, 29, 6, wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(f::openJoinAccept(factoryMac, joinDown, wire, size, accept));
    TEST_ASSERT_FALSE(f::openJoinAccept(factoryMac, joinDown, nullptr, size, accept));
    TEST_ASSERT_FALSE(f::openJoinAccept(factoryMac, joinDown, wire, size - 1, accept));
}

void test_command_payloads_and_encrypted_known_answers() {
    f::Command command; command.commandId = 0x1234; command.type = 1; command.argumentSize = 4;
    const uint8_t arguments[] = {0x44,0x33,0x22,0x11}; memcpy(command.arguments, arguments, 4);
    uint8_t payload[11], wire[61]; size_t size = 0, wireSize = 0;
    TEST_ASSERT_TRUE(f::encodeCommand(command, payload, sizeof(payload), size));
    const uint8_t expected[] = {0x34,0x12,1,0x44,0x33,0x22,0x11};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, payload, size);
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kCommandHeader,
                             payload, size, wire, sizeof(wire), wireSize));
    vector("session_command", wire, wireSize);
    uint32_t decodedCounter; uint8_t* decodedPayload; size_t decodedSize;
    TEST_ASSERT_TRUE(s::open(context, s::Direction::Gateway, 7, down, wire, wireSize,
                            decodedCounter, decodedPayload, decodedSize));
    f::Command decoded;
    TEST_ASSERT_TRUE(f::decodeCommand(decodedPayload, decodedSize, decoded));
    TEST_ASSERT_EQUAL_HEX16(command.commandId, decoded.commandId);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(arguments, decoded.arguments, 4);
    f::CommandResult result; result.commandId = 0x1234; result.dataSize = 8;
    const uint8_t data[] = {0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55}; memcpy(result.data, data, 8);
    TEST_ASSERT_TRUE(f::encodeCommandResult(result, payload, sizeof(payload), size));
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Node, 7, up, counter, f::kCommandResultHeader,
                             payload, size, wire, sizeof(wire), wireSize));
    vector("session_result", wire, wireSize);
    TEST_ASSERT_TRUE(s::open(context, s::Direction::Node, 7, up, wire, wireSize,
                            decodedCounter, decodedPayload, decodedSize));
    f::CommandResult decodedResult;
    TEST_ASSERT_TRUE(f::decodeCommandResult(decodedPayload, decodedSize, decodedResult));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, decodedResult.data, 8);
}

void test_empty_ready_and_no_command_and_counter_binding() {
    uint8_t wire[61]; size_t size = 0;
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Node, 7, up, counter, f::kCommandReadyHeader,
                             nullptr, 0, wire, sizeof(wire), size));
    vector("session_ready", wire, size);
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kNoCommandHeader,
                             nullptr, 0, wire, sizeof(wire), size));
    vector("session_no_command", wire, size);
    uint8_t changed[61]; memcpy(changed, wire, size); changed[1] ^= 1;
    uint32_t decodedCounter; uint8_t* payload; size_t payloadSize;
    TEST_ASSERT_FALSE(s::open(context, s::Direction::Gateway, 7, down, changed, size,
                             decodedCounter, payload, payloadSize));
    TEST_ASSERT_TRUE(s::open(context, s::Direction::Gateway, 7, down, wire, size,
                            decodedCounter, payload, payloadSize));
    TEST_ASSERT_EQUAL_UINT32(counter, decodedCounter);
    TEST_ASSERT_EQUAL_UINT(0, payloadSize);
}

void test_command_bounds_unknown_type_and_statuses() {
    uint8_t payload[12]{}; size_t size = 999;
    f::Command command; command.commandId = 1; command.type = 255; command.argumentSize = 8;
    TEST_ASSERT_FALSE(f::encodeCommand(command, payload, 10, size));
    TEST_ASSERT_EQUAL_UINT(0, size);
    TEST_ASSERT_TRUE(f::encodeCommand(command, payload, sizeof(payload), size));
    f::Command decoded; decoded.commandId = 999;
    TEST_ASSERT_TRUE(f::decodeCommand(payload, size, decoded));
    TEST_ASSERT_EQUAL_UINT8(255, decoded.type);
    command.argumentSize = 9;
    TEST_ASSERT_FALSE(f::encodeCommand(command, payload, sizeof(payload), size));
    command.argumentSize = 0; command.commandId = 0;
    TEST_ASSERT_FALSE(f::encodeCommand(command, payload, sizeof(payload), size));
    TEST_ASSERT_FALSE(f::decodeCommand(payload, 12, decoded));
    TEST_ASSERT_FALSE(f::decodeCommand(payload, 2, decoded));
    TEST_ASSERT_FALSE(f::decodeCommand(nullptr, 3, decoded));
    payload[0] = payload[1] = 0;
    TEST_ASSERT_FALSE(f::decodeCommand(payload, 3, decoded));
    f::CommandResult result; result.commandId = 1;
    for (unsigned status = 0; status <= 3; ++status) {
        result.status = static_cast<p::CommandStatus>(status);
        TEST_ASSERT_TRUE(f::encodeCommandResult(result, payload, sizeof(payload), size));
        f::CommandResult decodedResult;
        TEST_ASSERT_TRUE(f::decodeCommandResult(payload, size, decodedResult));
        TEST_ASSERT_EQUAL_UINT8(status, static_cast<uint8_t>(decodedResult.status));
    }
    result.status = static_cast<p::CommandStatus>(4);
    TEST_ASSERT_FALSE(f::encodeCommandResult(result, payload, sizeof(payload), size));
    payload[2] = 4;
    TEST_ASSERT_FALSE(f::decodeCommandResult(payload, 3, result));
    result.status = p::CommandStatus::Applied; result.dataSize = 9;
    TEST_ASSERT_FALSE(f::encodeCommandResult(result, payload, sizeof(payload), size));
}

void test_valid_old_reply_and_authenticated_bad_payload_are_rejected() {
    uint8_t wire[61], original[61]; size_t size;
    const uint8_t payload[] = {1,0,2};
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kCommandHeader,
                             payload, sizeof(payload), wire, sizeof(wire), size));
    memcpy(original, wire, size);
    f::Command command; command.commandId = 999;
    bool hasCommand = false;
    TEST_ASSERT_FALSE(f::openCommandReply(context, 7, down, counter + 1, wire, size, command, hasCommand));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(original, wire, size);
    TEST_ASSERT_EQUAL_UINT16(999, command.commandId);
    TEST_ASSERT_TRUE(f::openCommandReply(context, 7, down, counter, wire, size, command, hasCommand));
    TEST_ASSERT_TRUE(hasCommand);
    TEST_ASSERT_EQUAL_UINT16(1, command.commandId);
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kNoCommandHeader,
                             payload, sizeof(payload), wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(f::openCommandReply(context, 7, down, counter, wire, size, command, hasCommand));
    TEST_ASSERT_TRUE(hasCommand);
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kNoCommandHeader,
                             nullptr, 0, wire, sizeof(wire), size));
    TEST_ASSERT_TRUE(f::openCommandReply(context, 7, down, counter, wire, size, command, hasCommand));
    TEST_ASSERT_FALSE(hasCommand);
    const uint8_t invalid[] = {0,0,1};
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kCommandHeader,
                             invalid, sizeof(invalid), wire, sizeof(wire), size));
    TEST_ASSERT_FALSE(f::openCommandReply(context, 7, down, counter, wire, size, command, hasCommand));
    TEST_ASSERT_EQUAL_UINT16(1, command.commandId);
}

void test_cached_reply_is_immutable_until_next_accepted_counter() {
    Memory memory; r::Store store(memory); store.load(); Random random;
    r::Guard guard(store, random); guard.restart();
    TEST_ASSERT_TRUE(guard.initializeFreshKeys(0));
    uint8_t noCommand[61], command[61], copied[61]; size_t noSize, commandSize, copiedSize;
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kNoCommandHeader,
                             nullptr, 0, noCommand, sizeof(noCommand), noSize));
    const uint8_t payload[] = {1,0,2};
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter, f::kCommandHeader,
                             payload, sizeof(payload), command, sizeof(command), commandSize));
    TEST_ASSERT_FALSE(guard.cacheReply(0, counter, noCommand, noSize));
    action(r::Action::Accept, guard.inspect(0, counter));
    TEST_ASSERT_TRUE(guard.cacheReply(0, counter, noCommand, noSize));
    TEST_ASSERT_TRUE(guard.cacheReply(0, counter, noCommand, noSize));
    TEST_ASSERT_FALSE(guard.cacheReply(0, counter, command, commandSize));
    action(r::Action::Duplicate, guard.inspect(0, counter));
    TEST_ASSERT_TRUE(guard.cachedReply(0, counter, copied, sizeof(copied), copiedSize));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(noCommand, copied, noSize);
    TEST_ASSERT_FALSE(guard.cachedReply(0, counter, copied, noSize - 1, copiedSize));
    TEST_ASSERT_EQUAL_UINT(0, copiedSize);
    TEST_ASSERT_FALSE(guard.cachedReply(1, counter, copied, sizeof(copied), copiedSize));
    TEST_ASSERT_FALSE(guard.cacheReply(0, counter + 1, command, commandSize));
    action(r::Action::Accept, guard.inspect(0, counter + 1));
    TEST_ASSERT_FALSE(guard.cachedReply(0, counter, copied, sizeof(copied), copiedSize));
    TEST_ASSERT_TRUE(s::seal(context, s::Direction::Gateway, 7, down, counter + 1, f::kCommandHeader,
                             payload, sizeof(payload), command, sizeof(command), commandSize));
    TEST_ASSERT_TRUE(guard.cacheReply(0, counter + 1, command, commandSize));
    store.load(); guard.restart();
    action(r::Action::CounterFloor, guard.inspect(0, counter + 1));
    TEST_ASSERT_FALSE(guard.cachedReply(0, counter + 1, copied, sizeof(copied), copiedSize));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_join_known_answers_and_full_nonce);
    RUN_TEST(test_pairing_every_bit_transport_and_salt_authenticated_before_output);
    RUN_TEST(test_join_bounds_and_authenticated_invalid_fields);
    RUN_TEST(test_command_payloads_and_encrypted_known_answers);
    RUN_TEST(test_empty_ready_and_no_command_and_counter_binding);
    RUN_TEST(test_command_bounds_unknown_type_and_statuses);
    RUN_TEST(test_valid_old_reply_and_authenticated_bad_payload_are_rejected);
    RUN_TEST(test_cached_reply_is_immutable_until_next_accepted_counter);
    return UNITY_END();
}
