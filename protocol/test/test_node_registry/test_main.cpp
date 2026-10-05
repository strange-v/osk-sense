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

uint8_t addRecord(NodeRegistry& registry, const JoinRequest& request) {
    NodeRecord records[kMaxNodes]{};
    const size_t count = registry.size();
    TEST_ASSERT_LESS_THAN_UINT32(kMaxNodes, count);
    memcpy(records, registry.records(), count * sizeof(NodeRecord));
    auto& record = records[count];
    memcpy(record.deviceUid, request.deviceUid, kDeviceUidSize);
    record.nodeId = static_cast<uint8_t>(count + 1);
    record.profileId = request.profileId;
    record.firmware = request.firmware;
    record.requestNonce = request.requestNonce;
    record.maxPowerLevel = request.maxPowerLevel;
    record.powerPolicy = radiosensors::radio_power::kPolicyAuto;
    record.state = NodeState::Pending;
    TEST_ASSERT_TRUE(registry.restore(records, count + 1));
    return record.nodeId;
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

void test_registry_capacity_is_bounded() {
    NodeRegistry registry;
    for (size_t index = 0; index < kMaxNodes; ++index) {
        addRecord(registry, makeRequest(static_cast<uint8_t>(index * 3), 1));
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxNodes, registry.size());
    TEST_ASSERT_FALSE(registry.restore(registry.records(), kMaxNodes + 1));
    TEST_ASSERT_EQUAL_UINT32(kMaxNodes, registry.size());
}

void test_snapshot_round_trip_preserves_records() {
    NodeRegistry source;
    const JoinRequest request = makeRequest(0x10, 0x1234, 0x89ABCDEF);
    addRecord(source, request);
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
    const uint8_t nodeId = addRecord(registry, request);
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


}

void test_update_info_replaces_the_identity_of_an_active_node() {
    namespace power = radiosensors::radio_power;
    NodeRegistry registry;
    JoinRequest request = makeRequest(0x10, 1, 7);
    request.maxPowerLevel = 20;
    const uint8_t nodeId = addRecord(registry, request);
    const NodeInfo info{1, FirmwareVersion{1, 2, 4}, 20};

    TEST_ASSERT_EQUAL(static_cast<int>(InfoStatus::NotFound),
                      static_cast<int>(registry.updateInfo(nodeId, request.deviceUid, info)));
    NodeRecord active = registry.records()[0];
    active.state = NodeState::Active;
    TEST_ASSERT_TRUE(registry.restore(&active, 1));
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
    addRecord(registry, makeRequest(0x10));
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
    addRecord(source, makeRequest(0x10));
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
    addRecord(registry, makeRequest(0x10));
    TEST_ASSERT_TRUE(writer.save(registry));
    addRecord(registry, makeRequest(0x30));
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
    addRecord(registry, makeRequest(0x10));
    TEST_ASSERT_TRUE(writer.save(registry));
    addRecord(registry, makeRequest(0x30));
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
