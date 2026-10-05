#include <JoinRequest.h>
#include <NodeRegistry.h>
#include <RadioPowerControl.h>
#include <RegistryPersistence.h>
#include <unity.h>
#include <string.h>

using namespace radiosensors::protocol;
using namespace radiosensors::registry;

namespace {

JoinRequest makeRequest(
    const uint8_t uidSeed,
    const uint16_t profileId = 1,
    const uint32_t nonce = 1) {
    JoinRequest request{};
    for (size_t index = 0; index < kDeviceUidSize; ++index) {
        request.deviceUid[index] = static_cast<uint8_t>(uidSeed + index);
    }
    request.profileId = profileId;
    request.firmware = FirmwareVersion{1, 2, 3};
    request.requestNonce = nonce;
    return request;
}

class MemoryStorage final : public radiosensors::replay::BlobStorage {
public:
    radiosensors::replay::ReadStatus read(
        uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (size_ == 0) return radiosensors::replay::ReadStatus::Missing;
        if (size_ > capacity) return radiosensors::replay::ReadStatus::Invalid;
        memcpy(output, data_, size_);
        size = size_;
        return radiosensors::replay::ReadStatus::Ok;
    }
    bool write(const uint8_t* data, size_t size) override {
        if (!data || size > sizeof(data_)) return false;
        memcpy(data_, data, size);
        size_ = size;
        return true;
    }
    void corrupt(size_t offset) {
        if (offset < size_) data_[offset] ^= 0x80;
    }
private:
    uint8_t data_[kMaxRegistrySnapshotSize]{};
    size_t size_ = 0;
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_reserves_stable_unique_ids() {
    NodeRegistry registry;
    const JoinRequest first = makeRequest(0x10, 0x1001, 0x11111111);
    const JoinRequest second = makeRequest(0x30, 0x1002, 0x22222222);

    const ReserveResult firstResult = registry.reserve(first);
    const ReserveResult secondResult = registry.reserve(second);
    TEST_ASSERT_EQUAL(static_cast<int>(ReserveStatus::Created),
                      static_cast<int>(firstResult.status));
    TEST_ASSERT_EQUAL_UINT8(1, firstResult.nodeId);
    TEST_ASSERT_EQUAL_UINT8(2, secondResult.nodeId);
    TEST_ASSERT_EQUAL_UINT32(2, registry.size());

    JoinRequest retry = first;
    retry.requestNonce = 0x33333333;
    const ReserveResult retryResult = registry.reserve(retry);
    TEST_ASSERT_EQUAL(static_cast<int>(ReserveStatus::ExistingPendingUpdated),
                      static_cast<int>(retryResult.status));
    TEST_ASSERT_EQUAL_UINT8(firstResult.nodeId, retryResult.nodeId);
    TEST_ASSERT_EQUAL_HEX32(
        retry.requestNonce,
        registry.findByUid(first.deviceUid)->requestNonce);
}

void test_profile_conflict_does_not_change_record() {
    NodeRegistry registry;
    const JoinRequest first = makeRequest(0x10, 0x1001);
    registry.reserve(first);
    JoinRequest conflicting = first;
    conflicting.profileId = 0x1002;

    const ReserveResult result = registry.reserve(conflicting);
    TEST_ASSERT_EQUAL(static_cast<int>(ReserveStatus::ProfileConflict),
                      static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_HEX16(
        first.profileId,
        registry.findByUid(first.deviceUid)->profileId);
}

void test_confirm_requires_uid_node_id_and_latest_nonce() {
    NodeRegistry registry;
    const JoinRequest request = makeRequest(0x10, 0x1001, 0x12345678);
    const ReserveResult reserved = registry.reserve(request);
    const JoinRequest other = makeRequest(0x20, 0x1001, request.requestNonce);

    TEST_ASSERT_EQUAL(
        static_cast<int>(ConfirmStatus::IdentityMismatch),
        static_cast<int>(registry.confirm(
            other.deviceUid, reserved.nodeId, request.requestNonce)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(ConfirmStatus::NonceMismatch),
        static_cast<int>(registry.confirm(
            request.deviceUid, reserved.nodeId, request.requestNonce + 1)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(ConfirmStatus::Confirmed),
        static_cast<int>(registry.confirm(
            request.deviceUid, reserved.nodeId, request.requestNonce)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(NodeState::Active),
        static_cast<int>(registry.findByNodeId(reserved.nodeId)->state));
    TEST_ASSERT_EQUAL(
        static_cast<int>(ConfirmStatus::AlreadyActive),
        static_cast<int>(registry.confirm(
            request.deviceUid, reserved.nodeId, request.requestNonce)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(ConfirmStatus::NonceMismatch),
        static_cast<int>(registry.confirm(
            request.deviceUid, reserved.nodeId, request.requestNonce + 1)));
}

void test_registry_capacity_is_bounded() {
    NodeRegistry registry;
    for (size_t index = 0; index < kMaxNodes; ++index) {
        const JoinRequest request = makeRequest(static_cast<uint8_t>(index * 3), 1);
        TEST_ASSERT_EQUAL(
            static_cast<int>(ReserveStatus::Created),
            static_cast<int>(registry.reserve(request).status));
    }

    const JoinRequest overflow = makeRequest(0xF0, 1);
    TEST_ASSERT_EQUAL(
        static_cast<int>(ReserveStatus::Full),
        static_cast<int>(registry.reserve(overflow).status));
    TEST_ASSERT_EQUAL_UINT32(kMaxNodes, registry.size());
}

void test_snapshot_round_trip_preserves_records() {
    NodeRegistry source;
    const JoinRequest request = makeRequest(0x10, 0x1234, 0x89ABCDEF);
    source.reserve(request);
    const char name[] = "\xD0\x94\xD0\xB0\xD1\x82\xD1\x87\xD0\xB8\xD0\xBA";
    TEST_ASSERT_EQUAL(
        static_cast<int>(RenameStatus::Renamed),
        static_cast<int>(source.rename(1, name, sizeof(name) - 1)));
    uint8_t encoded[kMaxRegistrySnapshotSize]{};
    size_t encodedSize = 0;

    TEST_ASSERT_EQUAL(
        static_cast<int>(SnapshotStatus::Ok),
        static_cast<int>(encodeRegistrySnapshot(
            source, 42, encoded, sizeof(encoded), encodedSize)));

    NodeRegistry decoded;
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL(
        static_cast<int>(SnapshotStatus::Ok),
        static_cast<int>(decodeRegistrySnapshot(
            encoded, encodedSize, decoded, generation)));
    TEST_ASSERT_EQUAL_UINT32(42, generation);
    TEST_ASSERT_EQUAL_UINT32(1, decoded.size());
    const NodeRecord* record = decoded.findByUid(request.deviceUid);
    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_HEX16(request.profileId, record->profileId);
    TEST_ASSERT_EQUAL_HEX32(request.requestNonce, record->requestNonce);
    TEST_ASSERT_EQUAL_UINT8(sizeof(name) - 1, record->displayNameLength);
    TEST_ASSERT_EQUAL_MEMORY(name, record->displayName, sizeof(name) - 1);
}

void test_power_policy_is_bounded_by_the_ceiling_and_persisted() {
    namespace power = radiosensors::radio_power;
    NodeRegistry registry;
    JoinRequest request = makeRequest(0x10, 0x1234, 7);
    request.maxPowerLevel = 5;
    const uint8_t nodeId = registry.reserve(request).nodeId;
    const NodeRecord* record = registry.findByNodeId(nodeId);
    TEST_ASSERT_EQUAL_UINT8(5, record->maxPowerLevel);
    TEST_ASSERT_EQUAL_UINT8(power::kPolicyAuto, record->powerPolicy);

    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::InvalidPolicy),
                      static_cast<int>(registry.setPowerPolicy(nodeId, power::fixedPolicy(6))));
    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::InvalidPolicy),
                      static_cast<int>(registry.setPowerPolicy(nodeId, 40)));
    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::NotFound),
                      static_cast<int>(registry.setPowerPolicy(99, power::kPolicyAuto)));
    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::Updated),
                      static_cast<int>(registry.setPowerPolicy(nodeId, power::fixedPolicy(5))));
    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::NoChange),
                      static_cast<int>(registry.setPowerPolicy(nodeId, power::fixedPolicy(5))));

    uint8_t encoded[kMaxRegistrySnapshotSize]{};
    size_t encodedSize = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(SnapshotStatus::Ok),
                      static_cast<int>(encodeRegistrySnapshot(
                          registry, 3, encoded, sizeof(encoded), encodedSize)));
    TEST_ASSERT_EQUAL_UINT32(kRegistryHeaderSize + kStoredNodeRecordSize + kRegistryCrcSize,
                             encodedSize);
    NodeRegistry decoded;
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(SnapshotStatus::Ok),
                      static_cast<int>(decodeRegistrySnapshot(
                          encoded, encodedSize, decoded, generation)));
    TEST_ASSERT_EQUAL_UINT8(5, decoded.findByNodeId(nodeId)->maxPowerLevel);
    TEST_ASSERT_EQUAL_UINT8(power::fixedPolicy(5), decoded.findByNodeId(nodeId)->powerPolicy);

    // Pairing again with a lower ceiling returns a fixed level above it to
    // automatic control.
    request.maxPowerLevel = 3;
    TEST_ASSERT_EQUAL(static_cast<int>(ReserveStatus::ExistingPendingUpdated),
                      static_cast<int>(registry.reserve(request).status));
    TEST_ASSERT_EQUAL_UINT8(3, record->maxPowerLevel);
    TEST_ASSERT_EQUAL_UINT8(power::kPolicyAuto, record->powerPolicy);
}

void test_update_info_replaces_the_identity_of_an_active_node() {
    namespace power = radiosensors::radio_power;
    NodeRegistry registry;
    JoinRequest request = makeRequest(0x10, 1, 7);
    request.maxPowerLevel = 20;
    const uint8_t nodeId = registry.reserve(request).nodeId;
    const NodeInfo info{1, FirmwareVersion{1, 2, 4}, 20};

    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::NotFound),
                      static_cast<int>(registry.updateInfo(nodeId, request.deviceUid, info)));
    registry.confirm(request.deviceUid, nodeId, 7);
    const JoinRequest other = makeRequest(0x30);
    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::NotFound),
                      static_cast<int>(registry.updateInfo(nodeId, other.deviceUid, info)));
    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::NotFound),
                      static_cast<int>(registry.updateInfo(99, request.deviceUid, info)));

    TEST_ASSERT_EQUAL(static_cast<int>(PowerPolicyStatus::Updated),
                      static_cast<int>(registry.setPowerPolicy(nodeId, power::fixedPolicy(10))));
    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::Updated),
                      static_cast<int>(registry.updateInfo(nodeId, request.deviceUid, info)));
    const NodeRecord* record = registry.findByNodeId(nodeId);
    TEST_ASSERT_EQUAL_UINT8(4, record->firmware.patch);
    TEST_ASSERT_EQUAL_UINT8(power::fixedPolicy(10), record->powerPolicy);
    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::NoChange),
                      static_cast<int>(registry.updateInfo(nodeId, request.deviceUid, info)));

    // A new profile with a lower ceiling returns the fixed level to automatic
    // control.
    const NodeInfo reflashed{6, FirmwareVersion{1, 2, 4}, 5};
    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::ProfileChanged),
                      static_cast<int>(registry.updateInfo(nodeId, request.deviceUid, reflashed)));
    TEST_ASSERT_EQUAL_UINT16(6, record->profileId);
    TEST_ASSERT_EQUAL_UINT8(5, record->maxPowerLevel);
    TEST_ASSERT_EQUAL_UINT8(power::kPolicyAuto, record->powerPolicy);
}

void test_rename_accepts_utf8_and_rejects_invalid_names() {
    NodeRegistry registry;
    registry.reserve(makeRequest(0x10));
    const char ukrainian[] = "\xD0\x9A\xD1\x96\xD0\xBC\xD0\xBD\xD0\xB0\xD1\x82\xD0\xB0";
    TEST_ASSERT_EQUAL(
        static_cast<int>(RenameStatus::Renamed),
        static_cast<int>(registry.rename(1, ukrainian, sizeof(ukrainian) - 1)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(RenameStatus::NoChange),
        static_cast<int>(registry.rename(1, ukrainian, sizeof(ukrainian) - 1)));
    const char invalid[] = "\xC0\xAF";
    TEST_ASSERT_EQUAL(
        static_cast<int>(RenameStatus::InvalidName),
        static_cast<int>(registry.rename(1, invalid, sizeof(invalid) - 1)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(RenameStatus::NotFound),
        static_cast<int>(registry.rename(2, "other", 5)));
}

void test_snapshot_rejects_crc_corruption() {
    NodeRegistry source;
    source.reserve(makeRequest(0x10));
    uint8_t encoded[kMaxRegistrySnapshotSize]{};
    size_t encodedSize = 0;
    encodeRegistrySnapshot(source, 1, encoded, sizeof(encoded), encodedSize);
    encoded[kRegistryHeaderSize] ^= 0x01;

    NodeRegistry decoded;
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL(
        static_cast<int>(SnapshotStatus::CrcMismatch),
        static_cast<int>(decodeRegistrySnapshot(
            encoded, encodedSize, decoded, generation)));
}

void test_atomic_store_blocks_corruption() {
    MemoryStorage slots;
    AtomicRegistryStore writer(slots);
    NodeRegistry registry;
    writer.load(registry);
    registry.reserve(makeRequest(0x10));
    TEST_ASSERT_TRUE(writer.save(registry));
    registry.reserve(makeRequest(0x30));
    TEST_ASSERT_TRUE(writer.save(registry));
    slots.corrupt(kRegistryHeaderSize);

    AtomicRegistryStore reader(slots);
    NodeRegistry recovered;
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoadStatus::Invalid),
        static_cast<int>(reader.load(recovered)));
    TEST_ASSERT_FALSE(reader.save(recovered));
    TEST_ASSERT_EQUAL_UINT32(0, recovered.size());
}

void test_atomic_store_loads_committed_generation() {
    MemoryStorage slots;
    AtomicRegistryStore writer(slots);
    NodeRegistry registry;
    writer.load(registry);
    registry.reserve(makeRequest(0x10));
    TEST_ASSERT_TRUE(writer.save(registry));
    registry.reserve(makeRequest(0x30));
    TEST_ASSERT_TRUE(writer.save(registry));

    AtomicRegistryStore reader(slots);
    NodeRegistry recovered;
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoadStatus::Loaded),
        static_cast<int>(reader.load(recovered)));
    TEST_ASSERT_EQUAL_UINT32(2, reader.generation());
    TEST_ASSERT_EQUAL_UINT32(2, recovered.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_reserves_stable_unique_ids);
    RUN_TEST(test_profile_conflict_does_not_change_record);
    RUN_TEST(test_confirm_requires_uid_node_id_and_latest_nonce);
    RUN_TEST(test_registry_capacity_is_bounded);
    RUN_TEST(test_snapshot_round_trip_preserves_records);
    RUN_TEST(test_power_policy_is_bounded_by_the_ceiling_and_persisted);
    RUN_TEST(test_update_info_replaces_the_identity_of_an_active_node);
    RUN_TEST(test_rename_accepts_utf8_and_rejects_invalid_names);
    RUN_TEST(test_snapshot_rejects_crc_corruption);
    RUN_TEST(test_atomic_store_blocks_corruption);
    RUN_TEST(test_atomic_store_loads_committed_generation);
    return UNITY_END();
}
