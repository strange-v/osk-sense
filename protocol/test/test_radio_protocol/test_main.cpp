#include <CommandSessionFrames.h>
#include <JoinRequest.h>
#include <ProfileIds.h>
#include <RadioProtocol.h>
#include <TelemetryFrames.h>
#include <GatewayStream.h>
#include <unity.h>

using namespace radiosensors::protocol;

namespace {

// Level 2 and a downlink RSSI of -70 dBm: `02 BA` in every example frame.
constexpr TelemetryPrefix kPrefix{3300, 0x02, -70};

} // namespace

void setUp() {
}
void tearDown() {
}

void test_header_bit_layout() {
    TEST_ASSERT_EQUAL_HEX8(0x40, encodeHeader(FrameKind::Telemetry));
    TEST_ASSERT_EQUAL_HEX8(0x41, encodeHeader(FrameKind::JoinRequest));
    TEST_ASSERT_EQUAL_HEX8(0x46, encodeHeader(FrameKind::Error));
    TEST_ASSERT_EQUAL_HEX8(0x48, encodeHeader(FrameKind::CommandReady));
    TEST_ASSERT_EQUAL_HEX8(0x49, encodeHeader(FrameKind::NoCommand));
}

void test_command_session_known_vectors() {
    const uint8_t readyExpected[] = {0x48, 0xEF, 0xCD, 0xAB, 0x89};
    const uint8_t noneExpected[] = {0x49, 0xEF, 0xCD, 0xAB, 0x89};
    uint8_t encoded[kCommandReadySize]{};
    uint32_t nonce = 0;

    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(encodeCommandReady(0x89ABCDEFUL, encoded, sizeof(encoded))));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(readyExpected, encoded, sizeof(encoded));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(decodeCommandReady(encoded, sizeof(encoded), nonce)));
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFUL, nonce);

    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(encodeNoCommand(0x89ABCDEFUL, encoded, sizeof(encoded))));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(noneExpected, encoded, sizeof(encoded));
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(decodeNoCommand(encoded, sizeof(encoded), nonce)));
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFUL, nonce);
}

void test_command_known_vectors() {
    const uint8_t countExpected[] =
        {0x44, 0xEF, 0xCD, 0xAB, 0x89, 0x35, 0x12, 0x01, 0x78, 0x56, 0x34, 0x12};
    uint8_t encoded[kMaxCommandSize]{};

    Command decoded{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(CommandSessionCodecStatus::Ok),
        static_cast<int>(decodeCommand(countExpected, sizeof(countExpected), decoded)));
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFUL, decoded.sessionNonce);
    TEST_ASSERT_EQUAL_HEX16(0x1235, decoded.commandId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandType::SetCount), decoded.type);
    TEST_ASSERT_EQUAL_UINT8(kSetCountArgumentSize, decoded.argumentSize);
    TEST_ASSERT_EQUAL_HEX32(0x12345678UL, readUint32Le(decoded.arguments));

    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(encodeCommand(decoded, encoded, sizeof(encoded))));
    TEST_ASSERT_EQUAL_UINT32(sizeof(countExpected), commandFrameSize(decoded));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(countExpected, encoded, sizeof(countExpected));
}

void test_command_result_known_vectors() {
    const uint8_t appliedExpected[] = {0x45,
                                       0xEF,
                                       0xCD,
                                       0xAB,
                                       0x89,
                                       0x35,
                                       0x12,
                                       0x00,
                                       0x10,
                                       0x00,
                                       0x00,
                                       0x00,
                                       0x78,
                                       0x56,
                                       0x34,
                                       0x12};
    const uint8_t rejectedExpected[] = {0x45, 0xEF, 0xCD, 0xAB, 0x89, 0x34, 0x12, 0x02};
    uint8_t encoded[kMaxCommandResultSize]{};

    CommandResult applied{0x89ABCDEFUL, 0x1235, CommandStatus::Applied, kSetCountResultSize, {}};
    writeUint32Le(applied.data, 16);
    writeUint32Le(applied.data + 4, 0x12345678UL);
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(encodeCommandResult(applied, encoded, sizeof(encoded))));
    TEST_ASSERT_EQUAL_UINT32(sizeof(appliedExpected), commandResultFrameSize(applied));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(appliedExpected, encoded, sizeof(appliedExpected));

    CommandResult decoded{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(CommandSessionCodecStatus::Ok),
        static_cast<int>(decodeCommandResult(rejectedExpected, sizeof(rejectedExpected), decoded)));
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEFUL, decoded.sessionNonce);
    TEST_ASSERT_EQUAL_HEX16(0x1234, decoded.commandId);
    TEST_ASSERT_EQUAL(static_cast<int>(CommandStatus::InvalidArgument),
                      static_cast<int>(decoded.status));
    TEST_ASSERT_EQUAL_UINT8(0, decoded.dataSize);
}

void test_command_frames_reject_invalid_envelopes() {
    Command command{};
    CommandResult result{};
    const uint8_t zeroId[] = {0x44, 1, 2, 3, 4, 0x00, 0x00, 0x01, 0x10};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::InvalidCommandId),
                      static_cast<int>(decodeCommand(zeroId, sizeof(zeroId), command)));

    const uint8_t tooShort[] = {0x44, 1, 2, 3, 4, 0x01, 0x00};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::WrongLength),
                      static_cast<int>(decodeCommand(tooShort, sizeof(tooShort), command)));

    uint8_t tooLong[kMaxCommandSize + 1] = {0x44, 1, 2, 3, 4, 0x01, 0x00, 0x02};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::WrongLength),
                      static_cast<int>(decodeCommand(tooLong, sizeof(tooLong), command)));

    const uint8_t wrongKind[] = {0x45, 1, 2, 3, 4, 0x01, 0x00, 0x00};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::WrongFrameKind),
                      static_cast<int>(decodeCommand(wrongKind, sizeof(wrongKind), command)));

    const uint8_t badStatus[] = {0x45, 1, 2, 3, 4, 0x01, 0x00, 0x04};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::InvalidStatus),
                      static_cast<int>(decodeCommandResult(badStatus, sizeof(badStatus), result)));

    const Command unassigned{1, 0, 1, 1, {0x10}};
    uint8_t encoded[kMaxCommandSize]{};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::InvalidCommandId),
                      static_cast<int>(encodeCommand(unassigned, encoded, sizeof(encoded))));
}

void test_unknown_command_type_decodes_for_an_unsupported_reply() {
    const uint8_t bytes[] = {0x44, 1, 2, 3, 4, 0x07, 0x00, 0x7F, 0xAA, 0xBB};
    Command command{};
    TEST_ASSERT_EQUAL(static_cast<int>(CommandSessionCodecStatus::Ok),
                      static_cast<int>(decodeCommand(bytes, sizeof(bytes), command)));
    TEST_ASSERT_EQUAL_UINT8(0x7F, command.type);
    TEST_ASSERT_EQUAL_UINT8(2, command.argumentSize);
    size_t size = 0;
    TEST_ASSERT_FALSE(commandArgumentSize(command.type, size));
}

void test_command_catalogue() {
    size_t size = 0;
    TEST_ASSERT_TRUE(commandArgumentSize(static_cast<uint8_t>(CommandType::SetCount), size));
    TEST_ASSERT_EQUAL_UINT32(4, size);
    TEST_ASSERT_TRUE(commandArgumentSize(static_cast<uint8_t>(CommandType::ReadInfo), size));
    TEST_ASSERT_EQUAL_UINT32(0, size);
    TEST_ASSERT_FALSE(commandArgumentSize(3, size));
    TEST_ASSERT_EQUAL_UINT32(8,
                             commandResultDataSize(CommandType::SetCount, CommandStatus::Applied));
    TEST_ASSERT_EQUAL_UINT32(
        0,
        commandResultDataSize(CommandType::SetCount, CommandStatus::InvalidArgument));
    TEST_ASSERT_EQUAL_UINT32(6,
                             commandResultDataSize(CommandType::ReadInfo, CommandStatus::Applied));

    const uint8_t count[4]{};
    TEST_ASSERT_TRUE(validCommandArguments(1, count, sizeof(count)));
    TEST_ASSERT_FALSE(validCommandArguments(1, count, 3));
    TEST_ASSERT_FALSE(validCommandArguments(1, nullptr, 4));
    TEST_ASSERT_TRUE(validCommandArguments(2, nullptr, 0));
    TEST_ASSERT_FALSE(validCommandArguments(2, count, 1));
    TEST_ASSERT_FALSE(validCommandArguments(3, nullptr, 0));

    for (uint16_t profile = 0; profile <= 9; ++profile) {
        TEST_ASSERT_EQUAL(profile == 6, profileSupportsCommand(profile, CommandType::SetCount));
        TEST_ASSERT_TRUE(profileSupportsCommand(profile, CommandType::ReadInfo));
    }
}

void test_node_info_known_vector() {
    const uint8_t expected[] = {0x06, 0x00, 0x01, 0x00, 0x02, 0x14};
    const NodeInfo info{6, FirmwareVersion{1, 0, 2}, 20};
    uint8_t encoded[kReadInfoResultSize]{};
    TEST_ASSERT_TRUE(encodeNodeInfo(info, encoded, sizeof(encoded)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, encoded, sizeof(expected));

    NodeInfo decoded{};
    TEST_ASSERT_TRUE(decodeNodeInfo(expected, sizeof(expected), decoded));
    TEST_ASSERT_EQUAL_UINT16(6, decoded.profileId);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.firmware.major);
    TEST_ASSERT_EQUAL_UINT8(0, decoded.firmware.minor);
    TEST_ASSERT_EQUAL_UINT8(2, decoded.firmware.patch);
    TEST_ASSERT_EQUAL_UINT8(20, decoded.maxPowerLevel);
}

void test_node_info_rejects_invalid_values() {
    uint8_t encoded[kReadInfoResultSize]{};
    TEST_ASSERT_FALSE(
        encodeNodeInfo(NodeInfo{0, FirmwareVersion{1, 0, 0}, 20}, encoded, sizeof(encoded)));
    TEST_ASSERT_FALSE(
        encodeNodeInfo(NodeInfo{6, FirmwareVersion{1, 0, 0}, 32}, encoded, sizeof(encoded)));
    TEST_ASSERT_FALSE(
        encodeNodeInfo(NodeInfo{6, FirmwareVersion{1, 0, 0}, 20}, encoded, sizeof(encoded) - 1));

    NodeInfo decoded{};
    const uint8_t zeroProfile[] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x14};
    const uint8_t highCeiling[] = {0x06, 0x00, 0x01, 0x00, 0x00, 0x20};
    const uint8_t valid[] = {0x06, 0x00, 0x01, 0x00, 0x00, 0x14};
    TEST_ASSERT_FALSE(decodeNodeInfo(zeroProfile, sizeof(zeroProfile), decoded));
    TEST_ASSERT_FALSE(decodeNodeInfo(highCeiling, sizeof(highCeiling), decoded));
    TEST_ASSERT_FALSE(decodeNodeInfo(valid, sizeof(valid) - 1, decoded));
}

void test_telemetry_ack_payload() {
    uint8_t payload[kMaxTelemetryAckSize]{};
    TEST_ASSERT_EQUAL_UINT32(
        0,
        encodeTelemetryAck(TelemetryAck{false, false, 0}, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT32(
        1,
        encodeTelemetryAck(TelemetryAck{true, false, 0}, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_HEX8(0x01, payload[0]);
    TEST_ASSERT_EQUAL_UINT32(
        2,
        encodeTelemetryAck(TelemetryAck{true, true, 17}, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_HEX8(0x03, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(17, payload[1]);
    TEST_ASSERT_EQUAL_UINT32(
        2,
        encodeTelemetryAck(TelemetryAck{false, true, 0}, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_HEX8(0x02, payload[0]);
    // An out-of-range target is dropped; the command flag survives.
    TEST_ASSERT_EQUAL_UINT32(
        1,
        encodeTelemetryAck(TelemetryAck{true, true, 32}, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_HEX8(0x01, payload[0]);

    const TelemetryAck empty = decodeTelemetryAck(nullptr, 0);
    TEST_ASSERT_FALSE(empty.commandPending);
    TEST_ASSERT_FALSE(empty.hasPowerTarget);
    const uint8_t both[] = {0x03, 17};
    const TelemetryAck decoded = decodeTelemetryAck(both, sizeof(both));
    TEST_ASSERT_TRUE(decoded.commandPending);
    TEST_ASSERT_TRUE(decoded.hasPowerTarget);
    TEST_ASSERT_EQUAL_UINT8(17, decoded.powerTarget);
    const uint8_t truncated[] = {0x02};
    TEST_ASSERT_FALSE(decodeTelemetryAck(truncated, sizeof(truncated)).hasPowerTarget);
    const uint8_t outOfRange[] = {0x02, 32};
    TEST_ASSERT_FALSE(decodeTelemetryAck(outOfRange, sizeof(outOfRange)).hasPowerTarget);
    const uint8_t reserved[] = {0xFC, 5};
    const TelemetryAck ignored = decodeTelemetryAck(reserved, sizeof(reserved));
    TEST_ASSERT_FALSE(ignored.commandPending);
    TEST_ASSERT_FALSE(ignored.hasPowerTarget);
}

void test_radio_state_and_downlink_rssi() {
    TEST_ASSERT_EQUAL_HEX8(0x02, encodeRadioState(2, false));
    TEST_ASSERT_EQUAL_HEX8(0x3F, encodeRadioState(31, true));
    TEST_ASSERT_EQUAL_UINT8(31, radioPowerLevel(0x3F));
    TEST_ASSERT_EQUAL_INT8(-70, downlinkRssiValue(-70));
    TEST_ASSERT_EQUAL_INT8(-127, downlinkRssiValue(-200));
    TEST_ASSERT_EQUAL_INT8(0, downlinkRssiValue(5));
}

void test_initial_profile_ids_are_stable() {
    TEST_ASSERT_EQUAL_UINT16(1, profileIdValue(ProfileId::Voltage));
    TEST_ASSERT_EQUAL_UINT16(2, profileIdValue(ProfileId::Temperature));
    TEST_ASSERT_EQUAL_UINT16(3, profileIdValue(ProfileId::ClimateTh));
    TEST_ASSERT_EQUAL_UINT16(4, profileIdValue(ProfileId::ClimateThp));
    TEST_ASSERT_EQUAL_UINT16(5, profileIdValue(ProfileId::Binary));
    TEST_ASSERT_EQUAL_UINT16(6, profileIdValue(ProfileId::PulseCounter));
    TEST_ASSERT_EQUAL_UINT16(7, profileIdValue(ProfileId::BinaryClimateTh));
    TEST_ASSERT_EQUAL_UINT16(8, profileIdValue(ProfileId::BinaryTemperature));
}

void test_decodes_opaque_telemetry() {
    const uint8_t bytes[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x05};
    FrameView frame{};
    TelemetryView telemetry{};

    TEST_ASSERT_EQUAL(static_cast<int>(DecodeStatus::Ok),
                      static_cast<int>(decodeFrame(bytes, sizeof(bytes), frame)));
    TEST_ASSERT_EQUAL(static_cast<int>(FrameKind::Telemetry), static_cast<int>(frame.kind));
    TEST_ASSERT_EQUAL_UINT32(5, frame.payloadSize);
    TEST_ASSERT_EQUAL_PTR(bytes + 1, frame.payload);
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::Ok),
                      static_cast<int>(decodeTelemetry(bytes, sizeof(bytes), telemetry)));
    TEST_ASSERT_EQUAL_UINT16(3300, telemetry.supplyMillivolts);
    TEST_ASSERT_EQUAL_HEX8(0x02, telemetry.radioState);
    TEST_ASSERT_EQUAL_INT8(-70, telemetry.downlinkRssi);
    TEST_ASSERT_EQUAL_UINT32(1, telemetry.profilePayloadSize);
    TEST_ASSERT_EQUAL_PTR(bytes + 5, telemetry.profilePayload);
    TEST_ASSERT_EQUAL_HEX8(0x05, telemetry.profilePayload[0]);
}

void test_profile_1_known_vector() {
    const uint8_t expected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C};
    uint8_t frame[kVoltageTelemetrySize]{};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::Ok),
                      static_cast<int>(encodeVoltageTelemetry(kPrefix, frame, sizeof(frame))));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
}

void test_profile_2_known_vector() {
    const uint8_t bytes[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x2E, 0x09};
    TelemetryView telemetry{};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::Ok),
                      static_cast<int>(decodeTelemetry(bytes, sizeof(bytes), telemetry)));
    TEST_ASSERT_EQUAL_UINT16(3300, telemetry.supplyMillivolts);
    TEST_ASSERT_EQUAL_UINT32(2, telemetry.profilePayloadSize);
    TEST_ASSERT_EQUAL_HEX16(0x092E, readUint16Le(telemetry.profilePayload));

    uint8_t frame[kTemperatureTelemetrySize]{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(encodeTemperatureTelemetry(kPrefix, 2350, frame, sizeof(frame))));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, frame, sizeof(bytes));
}

void test_initial_telemetry_encoders() {
    uint8_t frame[kBinaryClimateThTelemetrySize]{};

    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::Ok),
                      static_cast<int>(encodeBinaryTelemetry(kPrefix, 1, frame, sizeof(frame))));
    const uint8_t binaryExpected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(binaryExpected, frame, sizeof(binaryExpected));

    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(encodeCounterTelemetry(kPrefix, 0x12345678UL, frame, sizeof(frame))));
    const uint8_t counterExpected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(counterExpected, frame, sizeof(counterExpected));

    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(
            encodeBinaryClimateThTelemetry(kPrefix, 1, 2350, 4567, frame, sizeof(frame))));
    const uint8_t climateExpected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x01, 0x2E, 0x09, 0xD7, 0x11};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(climateExpected, frame, sizeof(climateExpected));

    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(encodeBinaryTemperatureTelemetry(kPrefix, 1, 2350, frame, sizeof(frame))));
    const uint8_t temperatureExpected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x01, 0x2E, 0x09};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(temperatureExpected, frame, sizeof(temperatureExpected));
}

void test_binary_telemetry_rejects_invalid_state() {
    uint8_t frame[kBinaryTelemetrySize]{};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::InvalidState),
                      static_cast<int>(encodeBinaryTelemetry(kPrefix, 2, frame, sizeof(frame))));
}

void test_climate_profile_known_vectors() {
    uint8_t th[kClimateThTelemetrySize]{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(encodeClimateThTelemetry(kPrefix, 2350, 4567, th, sizeof(th))));
    const uint8_t thExpected[] = {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x2E, 0x09, 0xD7, 0x11};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(thExpected, th, sizeof(th));

    uint8_t thp[kClimateThpTelemetrySize]{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::Ok),
        static_cast<int>(encodeClimateThpTelemetry(kPrefix, 2350, 4567, 10132, thp, sizeof(thp))));
    const uint8_t thpExpected[] =
        {0x40, 0x02, 0xBA, 0xE4, 0x0C, 0x2E, 0x09, 0xD7, 0x11, 0x94, 0x27};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(thpExpected, thp, sizeof(thp));
}

void test_climate_encoders_validate_measurements_and_allow_sentinels() {
    uint8_t frame[kClimateThpTelemetrySize]{};
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::InvalidTemperature),
        static_cast<int>(encodeClimateThTelemetry(kPrefix, -8001, 5000, frame, sizeof(frame))));
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::InvalidHumidity),
        static_cast<int>(encodeClimateThTelemetry(kPrefix, 2000, 10001, frame, sizeof(frame))));
    TEST_ASSERT_EQUAL(
        static_cast<int>(TelemetryCodecStatus::InvalidPressure),
        static_cast<int>(
            encodeClimateThpTelemetry(kPrefix, 2000, 5000, 2999, frame, sizeof(frame))));

    const TelemetryPrefix unknown{kInvalidSupplyVoltage,
                                  encodeRadioState(31, true),
                                  kNoDownlinkRssi};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::Ok),
                      static_cast<int>(encodeClimateThpTelemetry(unknown,
                                                                 kInvalidTemperature,
                                                                 kInvalidHumidity,
                                                                 kInvalidPressure,
                                                                 frame,
                                                                 sizeof(frame))));
    const uint8_t expected[] = {0x40, 0x3F, 0x80, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
}

void test_telemetry_prefix_rejects_wrong_length_header_and_radio_state() {
    TelemetryView telemetry{};
    const uint8_t tooShort[] = {0x40, 0x02, 0xBA, 0xE4};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::WrongLength),
                      static_cast<int>(decodeTelemetry(tooShort, sizeof(tooShort), telemetry)));

    const uint8_t wrongKind[] = {0x41, 0x02, 0xBA, 0xE4, 0x0C};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::WrongHeader),
                      static_cast<int>(decodeTelemetry(wrongKind, sizeof(wrongKind), telemetry)));

    const uint8_t reserved[] = {0x40, 0x82, 0xBA, 0xE4, 0x0C};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::InvalidRadioState),
                      static_cast<int>(decodeTelemetry(reserved, sizeof(reserved), telemetry)));

    uint8_t frame[kVoltageTelemetrySize]{};
    const TelemetryPrefix invalid{3300, 0x80, -70};
    TEST_ASSERT_EQUAL(static_cast<int>(TelemetryCodecStatus::InvalidRadioState),
                      static_cast<int>(encodeVoltageTelemetry(invalid, frame, sizeof(frame))));
}

void test_gateway_stream_known_vectors() {
    uint8_t control[radiosensors::stream::kSnapshotControlFrameSize]{};
    TEST_ASSERT_EQUAL_UINT32(sizeof(control),
                             radiosensors::stream::encodeSnapshotControl(
                                 radiosensors::stream::MessageKind::SnapshotBegin,
                                 0x78563412,
                                 0x01020304,
                                 control,
                                 sizeof(control)));
    const uint8_t expectedControl[] = {1, 2, 0x12, 0x34, 0x56, 0x78, 0x04, 0x03, 0x02, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedControl, control, sizeof(control));

    const uint8_t payload[] = {0x40, 0x02, 0xBA, 0xE7, 0x0C};
    uint8_t telemetry[radiosensors::stream::kTelemetryEnvelopeSize + sizeof(payload)]{};
    TEST_ASSERT_EQUAL_UINT32(sizeof(telemetry),
                             radiosensors::stream::encodeTelemetry(0x01020304,
                                                                   7,
                                                                   0x1234,
                                                                   0x0102030411223344ULL,
                                                                   -63,
                                                                   payload,
                                                                   sizeof(payload),
                                                                   telemetry,
                                                                   sizeof(telemetry)));
    const uint8_t expectedTelemetry[] = {1,    3,    4,    3,    2,    1,    7,    0x34, 0x12,
                                         0x44, 0x33, 0x22, 0x11, 0x04, 0x03, 0x02, 0x01, 0xC1,
                                         0xFF, 5,    0x40, 0x02, 0xBA, 0xE7, 0x0C};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedTelemetry, telemetry, sizeof(telemetry));
}

void test_gateway_stream_hello_known_vector() {
    const uint8_t gatewayId[radiosensors::stream::kIdentitySize] = {0x00,
                                                                    0x01,
                                                                    0x02,
                                                                    0x03,
                                                                    0x04,
                                                                    0x05,
                                                                    0x06,
                                                                    0x07,
                                                                    0x08,
                                                                    0x09,
                                                                    0x0A,
                                                                    0x0B,
                                                                    0x0C,
                                                                    0x0D,
                                                                    0x0E,
                                                                    0x0F};
    const uint8_t bootId[radiosensors::stream::kIdentitySize] = {0x10,
                                                                 0x11,
                                                                 0x12,
                                                                 0x13,
                                                                 0x14,
                                                                 0x15,
                                                                 0x16,
                                                                 0x17,
                                                                 0x18,
                                                                 0x19,
                                                                 0x1A,
                                                                 0x1B,
                                                                 0x1C,
                                                                 0x1D,
                                                                 0x1E,
                                                                 0x1F};
    uint8_t frame[radiosensors::stream::kHelloFrameSize]{};
    TEST_ASSERT_EQUAL_UINT32(sizeof(frame),
                             radiosensors::stream::encodeHello(0x01020304,
                                                               gatewayId,
                                                               bootId,
                                                               0x0000002A,
                                                               frame,
                                                               sizeof(frame)));
    const uint8_t expected[] = {1,    1,    0x04, 0x03, 0x02, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04,
                                0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                                0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
                                0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x2A, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(frame));
}

void test_gateway_stream_control_encoders_validate_input() {
    uint8_t snapshot[radiosensors::stream::kSnapshotControlFrameSize - 1]{};
    TEST_ASSERT_EQUAL_UINT32(0,
                             radiosensors::stream::encodeSnapshotControl(
                                 radiosensors::stream::MessageKind::SnapshotBegin,
                                 1,
                                 1,
                                 snapshot,
                                 sizeof(snapshot)));
    uint8_t hello[radiosensors::stream::kHelloFrameSize]{};
    const uint8_t identity[radiosensors::stream::kIdentitySize]{};
    TEST_ASSERT_EQUAL_UINT32(
        0,
        radiosensors::stream::encodeHello(1, nullptr, identity, 1, hello, sizeof(hello)));
}

void test_gateway_stream_registry_changed_known_vector() {
    uint8_t frame[radiosensors::stream::kRegistryChangedFrameSize]{};
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(frame),
        radiosensors::stream::encodeRegistryChanged(0x01020304, 0x0000002A, frame, sizeof(frame)));
    const uint8_t expected[] = {1, 5, 0x04, 0x03, 0x02, 0x01, 0x2A, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(frame));
}

void test_gateway_stream_registry_changed_rejects_small_buffer() {
    uint8_t frame[radiosensors::stream::kRegistryChangedFrameSize - 1]{};
    TEST_ASSERT_EQUAL_UINT32(
        0,
        radiosensors::stream::encodeRegistryChanged(1, 1, frame, sizeof(frame)));
}

void test_allows_empty_payload_for_kind_specific_validation() {
    const uint8_t bytes[] = {0x43};
    FrameView frame{};

    TEST_ASSERT_EQUAL(static_cast<int>(DecodeStatus::Ok),
                      static_cast<int>(decodeFrame(bytes, sizeof(bytes), frame)));
    TEST_ASSERT_EQUAL_UINT32(0, frame.payloadSize);
}

void test_rejects_empty_frame() {
    FrameView frame{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeStatus::EmptyFrame),
                      static_cast<int>(decodeFrame(nullptr, 0, frame)));
}

void test_rejects_other_protocol_major() {
    const uint8_t bytes[] = {0x20};
    FrameView frame{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeStatus::UnsupportedVersion),
                      static_cast<int>(decodeFrame(bytes, sizeof(bytes), frame)));
}

void test_rejects_reserved_kind() {
    const uint8_t bytes[] = {0x5F};
    FrameView frame{};
    TEST_ASSERT_EQUAL(static_cast<int>(DecodeStatus::UnsupportedKind),
                      static_cast<int>(decodeFrame(bytes, sizeof(bytes), frame)));
}

void test_encodes_join_request_known_vector() {
    const JoinRequest request{
        {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9},
        0x1234,
        {1, 2, 3},
        0x89ABCDEF,
        2,
    };
    const uint8_t expected[kJoinRequestSize] = {
        0x41, 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9,
        0x34, 0x12, 0x01, 0x02, 0x03, 0xEF, 0xCD, 0xAB, 0x89, 0x02,
    };
    uint8_t encoded[kJoinRequestSize]{};

    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::Ok),
                      static_cast<int>(encodeJoinRequest(request, encoded, sizeof(encoded))));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, encoded, kJoinRequestSize);
}

void test_decodes_join_request_known_vector() {
    const uint8_t encoded[kJoinRequestSize] = {
        0x41, 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9,
        0x34, 0x12, 0x01, 0x02, 0x03, 0xEF, 0xCD, 0xAB, 0x89, 0x02,
    };
    JoinRequest request{};

    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::Ok),
                      static_cast<int>(decodeJoinRequest(encoded, sizeof(encoded), request)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(encoded + 1, request.deviceUid, kDeviceUidSize);
    TEST_ASSERT_EQUAL_HEX16(0x1234, request.profileId);
    TEST_ASSERT_EQUAL_UINT8(1, request.firmware.major);
    TEST_ASSERT_EQUAL_UINT8(2, request.firmware.minor);
    TEST_ASSERT_EQUAL_UINT8(3, request.firmware.patch);
    TEST_ASSERT_EQUAL_HEX32(0x89ABCDEF, request.requestNonce);
    TEST_ASSERT_EQUAL_UINT8(2, request.maxPowerLevel);
}

void test_join_request_requires_exact_length() {
    uint8_t encoded[kJoinRequestSize + 1] = {0x41};
    encoded[11] = 1;
    JoinRequest request{};

    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::WrongLength),
                      static_cast<int>(decodeJoinRequest(encoded, sizeof(encoded), request)));
}

void test_join_request_rejects_profile_zero() {
    uint8_t encoded[kJoinRequestSize] = {0x41};
    JoinRequest request{};

    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::InvalidProfileId),
                      static_cast<int>(decodeJoinRequest(encoded, sizeof(encoded), request)));
}

void test_join_request_rejects_power_ceiling_above_31() {
    uint8_t encoded[kJoinRequestSize] = {0x41};
    encoded[11] = 1;
    encoded[20] = 32;
    JoinRequest request{};
    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::InvalidPowerLevel),
                      static_cast<int>(decodeJoinRequest(encoded, sizeof(encoded), request)));

    request.profileId = 1;
    request.maxPowerLevel = 32;
    TEST_ASSERT_EQUAL(static_cast<int>(JoinRequestStatus::InvalidPowerLevel),
                      static_cast<int>(encodeJoinRequest(request, encoded, sizeof(encoded))));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_header_bit_layout);
    RUN_TEST(test_command_session_known_vectors);
    RUN_TEST(test_command_known_vectors);
    RUN_TEST(test_command_result_known_vectors);
    RUN_TEST(test_command_frames_reject_invalid_envelopes);
    RUN_TEST(test_unknown_command_type_decodes_for_an_unsupported_reply);
    RUN_TEST(test_command_catalogue);
    RUN_TEST(test_node_info_known_vector);
    RUN_TEST(test_node_info_rejects_invalid_values);
    RUN_TEST(test_telemetry_ack_payload);
    RUN_TEST(test_radio_state_and_downlink_rssi);
    RUN_TEST(test_initial_profile_ids_are_stable);
    RUN_TEST(test_decodes_opaque_telemetry);
    RUN_TEST(test_profile_1_known_vector);
    RUN_TEST(test_profile_2_known_vector);
    RUN_TEST(test_initial_telemetry_encoders);
    RUN_TEST(test_binary_telemetry_rejects_invalid_state);
    RUN_TEST(test_climate_profile_known_vectors);
    RUN_TEST(test_climate_encoders_validate_measurements_and_allow_sentinels);
    RUN_TEST(test_telemetry_prefix_rejects_wrong_length_header_and_radio_state);
    RUN_TEST(test_gateway_stream_known_vectors);
    RUN_TEST(test_gateway_stream_hello_known_vector);
    RUN_TEST(test_gateway_stream_control_encoders_validate_input);
    RUN_TEST(test_gateway_stream_registry_changed_known_vector);
    RUN_TEST(test_gateway_stream_registry_changed_rejects_small_buffer);
    RUN_TEST(test_allows_empty_payload_for_kind_specific_validation);
    RUN_TEST(test_rejects_empty_frame);
    RUN_TEST(test_rejects_other_protocol_major);
    RUN_TEST(test_rejects_reserved_kind);
    RUN_TEST(test_encodes_join_request_known_vector);
    RUN_TEST(test_decodes_join_request_known_vector);
    RUN_TEST(test_join_request_requires_exact_length);
    RUN_TEST(test_join_request_rejects_profile_zero);
    RUN_TEST(test_join_request_rejects_power_ceiling_above_31);
    return UNITY_END();
}
