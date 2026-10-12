#include <CommissioningFrames.h>
#include <unity.h>

using namespace radiosensors::protocol;

void setUp() {
}
void tearDown() {
}

void test_join_accept_known_vector_round_trip() {
    JoinAccept source{};
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        source.deviceUid[i] = i;
    source.requestNonce = 0x12345678;
    source.assignedNodeId = 7;
    source.gatewayNodeId = 100;
    source.networkId = 128;
    for (size_t i = 0; i < kInstallationKeySize; ++i)
        source.installationKey[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t bytes[kJoinAcceptSize]{};
    const uint8_t expectedNonce[] = {0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(encodeJoinAccept(source, bytes, sizeof(bytes))));
    TEST_ASSERT_EQUAL_HEX8(0x42, bytes[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(source.deviceUid, bytes + 1, kDeviceUidSize);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedNonce, bytes + 11, 4);
    TEST_ASSERT_EQUAL_UINT8(7, bytes[15]);
    TEST_ASSERT_EQUAL_UINT8(100, bytes[16]);
    TEST_ASSERT_EQUAL_UINT8(128, bytes[17]);

    JoinAccept decoded{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(decodeJoinAccept(bytes, sizeof(bytes), decoded)));
    TEST_ASSERT_EQUAL_HEX32(source.requestNonce, decoded.requestNonce);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(source.installationKey,
                                 decoded.installationKey,
                                 kInstallationKeySize);
}

void test_join_accept_rejects_invalid_ids() {
    JoinAccept value{};
    value.assignedNodeId = 100;
    value.gatewayNodeId = 100;
    uint8_t bytes[kJoinAcceptSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::InvalidNodeId),
                          static_cast<int>(encodeJoinAccept(value, bytes, sizeof(bytes))));
}

void test_join_confirm_known_vector_round_trip() {
    JoinConfirm source{};
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        source.deviceUid[i] = 0xF0 + i;
    source.requestNonce = 0x89ABCDEF;
    uint8_t bytes[kJoinConfirmSize]{};
    const uint8_t expectedNonce[] = {0xEF, 0xCD, 0xAB, 0x89};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(encodeJoinConfirm(source, bytes, sizeof(bytes))));
    TEST_ASSERT_EQUAL_HEX8(0x43, bytes[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedNonce, bytes + 11, 4);

    JoinConfirm decoded{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(decodeJoinConfirm(bytes, sizeof(bytes), decoded)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(source.deviceUid, decoded.deviceUid, kDeviceUidSize);
    TEST_ASSERT_EQUAL_HEX32(source.requestNonce, decoded.requestNonce);
}

void test_join_complete_known_vector_round_trip() {
    JoinComplete source{};
    for (size_t i = 0; i < kDeviceUidSize; ++i)
        source.deviceUid[i] = 0xA0 + i;
    source.requestNonce = 0x10203040;
    uint8_t bytes[kJoinCompleteSize]{};

    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(encodeJoinComplete(source, bytes, sizeof(bytes))));
    TEST_ASSERT_EQUAL_HEX8(0x47, bytes[0]);

    JoinComplete decoded{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissioningCodecStatus::Ok),
                          static_cast<int>(decodeJoinComplete(bytes, sizeof(bytes), decoded)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(source.deviceUid, decoded.deviceUid, kDeviceUidSize);
    TEST_ASSERT_EQUAL_HEX32(source.requestNonce, decoded.requestNonce);
}

void test_commissioning_frames_require_exact_length_and_kind() {
    uint8_t accept[kJoinAcceptSize] = {0x42};
    accept[15] = 1;
    accept[16] = 100;
    JoinAccept decodedAccept{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CommissioningCodecStatus::WrongLength),
        static_cast<int>(decodeJoinAccept(accept, sizeof(accept) - 1, decodedAccept)));
    accept[0] = 0x43;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(CommissioningCodecStatus::WrongFrameKind),
        static_cast<int>(decodeJoinAccept(accept, sizeof(accept), decodedAccept)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_join_accept_known_vector_round_trip);
    RUN_TEST(test_join_accept_rejects_invalid_ids);
    RUN_TEST(test_join_confirm_known_vector_round_trip);
    RUN_TEST(test_join_complete_known_vector_round_trip);
    RUN_TEST(test_commissioning_frames_require_exact_length_and_kind);
    return UNITY_END();
}
