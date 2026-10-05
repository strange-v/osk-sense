#include <GatewayStorage.h>
#include <RadioProtocol.h>
#include <UserManagement.h>
#include <unity.h>

#include <string.h>

using namespace radiosensors::gateway_storage;
namespace user_management = radiosensors::user_management;

namespace {

class MemorySlots final : public SlotStorage {
public:
    bool exists(uint8_t slot) override { return slot < 2 && present_[slot]; }

    bool read(uint8_t slot, uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (slot >= 2 || !present_[slot] || sizes_[slot] > capacity || failReads_) return false;
        memcpy(output, data_[slot], sizes_[slot]);
        size = sizes_[slot];
        return true;
    }

    bool write(uint8_t slot, const uint8_t* data, size_t size) override {
        ++writeCount_;
        if (slot >= 2 || data == nullptr || size > kAuthSnapshotSize || failWrites_) return false;
        memcpy(data_[slot], data, size);
        sizes_[slot] = size;
        present_[slot] = true;
        if (corruptAfterWrite_) data_[slot][12] ^= 0x80;
        return true;
    }

    void corrupt(uint8_t slot, size_t offset) { data_[slot][offset] ^= 0x80; }
    void failWrites(bool value) { failWrites_ = value; }
    void corruptAfterWrite(bool value) { corruptAfterWrite_ = value; }
    uint32_t writeCount() const { return writeCount_; }

private:
    uint8_t data_[2][kAuthSnapshotSize]{};
    size_t sizes_[2]{};
    bool present_[2]{};
    bool failReads_ = false;
    bool failWrites_ = false;
    bool corruptAfterWrite_ = false;
    uint32_t writeCount_ = 0;
};

GatewaySettings namedSettings(const char* name) {
    GatewaySettings value = defaultSettings();
    value.hostnameLength = static_cast<uint8_t>(strlen(name));
    memcpy(value.hostname, name, value.hostnameLength);
    return value;
}

AuthenticationData populatedAuthentication() {
    AuthenticationData value = defaultAuthentication();
    value.userCount = 1;
    value.nextUserId = 2;
    UserRecord& user = value.users[0];
    user.id = 1;
    memcpy(user.username, "admin", 5);
    user.usernameLength = 5;
    user.role = UserRole::Admin;
    user.enabled = true;
    user.hashAlgorithm = PasswordHashAlgorithm::Pbkdf2HmacSha256;
    user.pbkdf2Iterations = 100000;
    for (size_t index = 0; index < kPasswordSaltSize; ++index) user.salt[index] = index + 1;
    for (size_t index = 0; index < kPasswordHashSize; ++index) user.passwordHash[index] = index + 2;

    value.tokenCount = 1;
    value.nextTokenId = 2;
    ApiTokenRecord& token = value.tokens[0];
    token.id = 1;
    memcpy(token.name, "home-assistant", 14);
    token.nameLength = 14;
    token.enabled = true;
    token.scopes = TelemetryRead;
    token.createdAtUnixMs = 0x0102030405060708ULL;
    for (size_t index = 0; index < kTokenHashSize; ++index) token.tokenHash[index] = index + 3;
    return value;
}

InstallationSecrets populatedSecrets() {
    InstallationSecrets value{};
    value.radioConfigured = true;
    value.deviceSecretPresent = true;
    value.operationalNetworkId = 128;
    for (size_t index = 0; index < kDeviceSecretSize; ++index) value.deviceSecret[index] = index + 32;
    return value;
}

user_management::PasswordCredential credential(uint8_t seed) {
    user_management::PasswordCredential value{};
    value.algorithm = PasswordHashAlgorithm::Pbkdf2HmacSha256;
    value.iterations = 100000;
    for (size_t index = 0; index < sizeof(value.salt); ++index)
        value.salt[index] = seed + index;
    for (size_t index = 0; index < sizeof(value.hash); ++index)
        value.hash[index] = seed + index + 1;
    return value;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_empty_stores_return_documented_defaults() {
    MemorySlots slots;
    SettingsStore settingsStore(slots);
    GatewaySettings settings{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Empty), static_cast<int>(settingsStore.load(settings)));
    TEST_ASSERT_TRUE(settingsEqual(defaultSettings(), settings));

    AuthenticationStore authStore(slots);
    AuthenticationData auth{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Empty), static_cast<int>(authStore.load(auth)));
    TEST_ASSERT_TRUE(authenticationEqual(defaultAuthentication(), auth));
    TEST_ASSERT_EQUAL_UINT32(1, auth.nextUserId);
    TEST_ASSERT_EQUAL_UINT32(1, auth.nextTokenId);
}

void test_settings_known_layout_and_round_trip() {
    const GatewaySettings source = namedSettings("main-gateway");
    uint8_t bytes[kSettingsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(encodeSettings(source, 0x12345678, bytes, sizeof(bytes))));
    TEST_ASSERT_EQUAL_MEMORY("RSGC", bytes, 4);
    TEST_ASSERT_EQUAL_HEX8(0x78, bytes[6]);
    TEST_ASSERT_EQUAL_HEX8(0x56, bytes[7]);
    TEST_ASSERT_EQUAL_UINT8(3, bytes[12]);
    TEST_ASSERT_EQUAL_UINT8(12, bytes[13]);
    TEST_ASSERT_EQUAL_MEMORY("main-gateway", bytes + 20, 12);
    TEST_ASSERT_EQUAL_UINT8(12, bytes[52]);
    TEST_ASSERT_EQUAL_MEMORY("pool.ntp.org", bytes + 53, 12);

    GatewaySettings decoded{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(decodeSettings(bytes, sizeof(bytes), decoded, generation)));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, generation);
    TEST_ASSERT_TRUE(settingsEqual(source, decoded));
}

// The hostname reaches DNS and DHCP unchanged, so the codec is the last place
// that can stop a name those two would resolve differently.
void test_settings_reject_names_dns_cannot_carry() {
    uint8_t bytes[kSettingsSnapshotSize]{};
    const char* rejected[] = {"Main-Gateway", "-hub", "hub-", "osk hub", "hub_1", "hub.local"};
    for (const char* name : rejected) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CodecStatus::InvalidString),
            static_cast<int>(encodeSettings(namedSettings(name), 1, bytes, sizeof(bytes))));
    }
    const char* accepted[] = {"", "osk-hub-floor1", "a", "1"};
    for (const char* name : accepted) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CodecStatus::Ok),
            static_cast<int>(encodeSettings(namedSettings(name), 1, bytes, sizeof(bytes))));
    }
}

void test_authentication_and_secrets_round_trip() {
    const AuthenticationData auth = populatedAuthentication();
    uint8_t authBytes[kAuthSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(encodeAuthentication(auth, 7, authBytes, sizeof(authBytes))));
    TEST_ASSERT_EQUAL_MEMORY("RSAU", authBytes, 4);
    TEST_ASSERT_EQUAL_MEMORY("admin", authBytes + 29, 5);
    TEST_ASSERT_EQUAL_HEX8(0x08, authBytes[392 + 40]);
    TEST_ASSERT_EQUAL_HEX8(0x01, authBytes[392 + 47]);
    AuthenticationData decodedAuth{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(decodeAuthentication(authBytes, sizeof(authBytes), decodedAuth, generation)));
    TEST_ASSERT_TRUE(authenticationEqual(auth, decodedAuth));

    const InstallationSecrets secrets = populatedSecrets();
    uint8_t secretBytes[kSecretsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(encodeSecrets(secrets, 8, secretBytes, sizeof(secretBytes))));
    TEST_ASSERT_EQUAL_MEMORY("RSGS", secretBytes, 4);
    InstallationSecrets decodedSecrets{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(decodeSecrets(secretBytes, sizeof(secretBytes), decodedSecrets, generation)));
    TEST_ASSERT_TRUE(secretsEqual(secrets, decodedSecrets));
}

void test_secrets_known_layout() {
    const InstallationSecrets source = populatedSecrets();
    uint8_t bytes[kSecretsSnapshotSize]{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(encodeSecrets(source, 8, bytes, sizeof(bytes))));
    TEST_ASSERT_EQUAL_UINT8(51, bytes[10]);
    TEST_ASSERT_EQUAL_UINT16(kSecretsStorageVersion, radiosensors::protocol::readUint16Le(bytes + 4));
    TEST_ASSERT_EQUAL_UINT8(0, bytes[11]);
    TEST_ASSERT_EQUAL_HEX8(0x03, bytes[12]);
    TEST_ASSERT_EQUAL_HEX8(0x00, bytes[13]);
    TEST_ASSERT_EQUAL_UINT8(128, bytes[14]);
    TEST_ASSERT_EQUAL_MEMORY(source.deviceSecret, bytes + 15, kDeviceSecretSize);
}

// A network ID is stored before setup; operational network 0 is always refused.
void test_secrets_network_id_is_never_zero() {
    uint8_t bytes[kSecretsSnapshotSize]{};
    InstallationSecrets bootstrap{};
    bootstrap.deviceSecretPresent = true;
    bootstrap.deviceSecret[0] = 1;
    bootstrap.operationalNetworkId = 42;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(encodeSecrets(bootstrap, 1, bytes, sizeof(bytes))));
    InstallationSecrets decoded{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::Ok), static_cast<int>(decodeSecrets(bytes, sizeof(bytes), decoded, generation)));
    TEST_ASSERT_TRUE(secretsEqual(bootstrap, decoded));

    bootstrap.operationalNetworkId = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::InvalidValue), static_cast<int>(encodeSecrets(bootstrap, 1, bytes, sizeof(bytes))));
}

void test_validation_rejects_invalid_relationships() {
    uint8_t buffer[kAuthSnapshotSize]{};
    AuthenticationData auth = populatedAuthentication();
    auth.users[0].role = UserRole::Viewer;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::MissingAdmin), static_cast<int>(encodeAuthentication(auth, 1, buffer, sizeof(buffer))));
    auth = populatedAuthentication();
    auth.nextUserId = 1;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::InvalidValue), static_cast<int>(encodeAuthentication(auth, 1, buffer, sizeof(buffer))));

    InstallationSecrets secrets = populatedSecrets();
    secrets.operationalNetworkId = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::InvalidValue), static_cast<int>(encodeSecrets(secrets, 1, buffer, sizeof(buffer))));
}

void test_crc_corruption_is_rejected() {
    const GatewaySettings source = defaultSettings();
    uint8_t bytes[kSettingsSnapshotSize]{};
    encodeSettings(source, 1, bytes, sizeof(bytes));
    bytes[20] ^= 1;
    GatewaySettings decoded{};
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CodecStatus::CrcMismatch), static_cast<int>(decodeSettings(bytes, sizeof(bytes), decoded, generation)));
}

void test_store_distinguishes_empty_from_existing_corrupt_slots() {
    MemorySlots slots;
    SettingsStore writer(slots);
    TEST_ASSERT_TRUE(writer.save(defaultSettings()));
    slots.corrupt(0, 20);

    SettingsStore reader(slots);
    GatewaySettings value{};
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(LoadStatus::Corrupt),
        static_cast<int>(reader.load(value)));
    TEST_ASSERT_TRUE(settingsEqual(defaultSettings(), value));
    TEST_ASSERT_EQUAL_UINT32(0, reader.generation());
}

void test_dual_slot_falls_back_and_identical_save_is_noop() {
    MemorySlots slots;
    SettingsStore writer(slots);
    GatewaySettings first = namedSettings("first");
    GatewaySettings second = namedSettings("second");
    TEST_ASSERT_TRUE(writer.save(first));
    TEST_ASSERT_TRUE(writer.save(first));
    TEST_ASSERT_EQUAL_UINT32(1, slots.writeCount());
    TEST_ASSERT_TRUE(writer.save(second));
    slots.corrupt(1, 20);

    SettingsStore reader(slots);
    GatewaySettings recovered{};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LoadStatus::Loaded), static_cast<int>(reader.load(recovered)));
    TEST_ASSERT_EQUAL_UINT32(1, reader.generation());
    TEST_ASSERT_TRUE(settingsEqual(first, recovered));
}

void test_failed_or_unverifiable_write_does_not_publish_generation() {
    MemorySlots slots;
    SettingsStore store(slots);
    TEST_ASSERT_TRUE(store.save(defaultSettings()));
    slots.corruptAfterWrite(true);
    TEST_ASSERT_FALSE(store.save(namedSettings("changed")));
    TEST_ASSERT_EQUAL_UINT32(1, store.generation());
}

void test_user_management_create_and_validation() {
    AuthenticationData data = populatedAuthentication();
    const auto password = credential(10);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::create(
            data, "sensor.viewer", 13, UserRole::Viewer, true, password, id)));
    TEST_ASSERT_EQUAL_UINT32(2, id);
    TEST_ASSERT_EQUAL_UINT8(2, data.userCount);
    TEST_ASSERT_EQUAL_MEMORY("sensor.viewer", data.users[1].username, 13);
    TEST_ASSERT_EQUAL_MEMORY(password.hash, data.users[1].passwordHash,
                             sizeof(password.hash));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::InvalidValue),
        static_cast<int>(user_management::create(
            data, "Uppercase", 9, UserRole::Viewer, true, password, id)));
}

void test_user_management_rejects_duplicate_and_capacity() {
    AuthenticationData data = populatedAuthentication();
    const auto password = credential(20);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::UsernameExists),
        static_cast<int>(user_management::create(
            data, "admin", 5, UserRole::Viewer, true, password, id)));
    TEST_ASSERT_EQUAL_UINT8(1, data.userCount);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::create(data, "one", 3, UserRole::Viewer, true, password, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::create(data, "two", 3, UserRole::Viewer, true, password, id)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::create(data, "three", 5, UserRole::Viewer, true, password, id)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::CapacityReached),
        static_cast<int>(user_management::create(
            data, "four", 4, UserRole::Viewer, true, password, id)));
}

void test_user_management_protects_last_admin() {
    AuthenticationData data = populatedAuthentication();
    const AuthenticationData original = data;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::LastAdminRequired),
        static_cast<int>(user_management::update(
            data, 1, "admin", 5, UserRole::Viewer, true)));
    TEST_ASSERT_TRUE(authenticationEqual(original, data));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::LastAdminRequired),
        static_cast<int>(user_management::update(
            data, 1, "admin", 5, UserRole::Admin, false)));
    TEST_ASSERT_TRUE(authenticationEqual(original, data));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(user_management::Status::LastAdminRequired),
        static_cast<int>(user_management::remove(data, 1)));
    TEST_ASSERT_TRUE(authenticationEqual(original, data));
}

void test_user_management_updates_password_and_removes_user() {
    AuthenticationData data = populatedAuthentication();
    const auto firstPassword = credential(30);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::create(
            data, "viewer", 6, UserRole::Viewer, true, firstPassword, id)));
    const auto replacement = credential(60);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::update(
            data, id, "renamed", 7, UserRole::Viewer, false, &replacement)));
    TEST_ASSERT_EQUAL_MEMORY(replacement.hash, data.users[1].passwordHash,
                             sizeof(replacement.hash));
    TEST_ASSERT_FALSE(data.users[1].enabled);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(user_management::Status::Ok),
        static_cast<int>(user_management::remove(data, id)));
    TEST_ASSERT_EQUAL_UINT8(1, data.userCount);
    TEST_ASSERT_EQUAL_UINT32(0, data.users[1].id);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_stores_return_documented_defaults);
    RUN_TEST(test_settings_known_layout_and_round_trip);
    RUN_TEST(test_settings_reject_names_dns_cannot_carry);
    RUN_TEST(test_authentication_and_secrets_round_trip);
    RUN_TEST(test_secrets_known_layout);
    RUN_TEST(test_secrets_network_id_is_never_zero);
    RUN_TEST(test_validation_rejects_invalid_relationships);
    RUN_TEST(test_crc_corruption_is_rejected);
    RUN_TEST(test_store_distinguishes_empty_from_existing_corrupt_slots);
    RUN_TEST(test_dual_slot_falls_back_and_identical_save_is_noop);
    RUN_TEST(test_failed_or_unverifiable_write_does_not_publish_generation);
    RUN_TEST(test_user_management_create_and_validation);
    RUN_TEST(test_user_management_rejects_duplicate_and_capacity);
    RUN_TEST(test_user_management_protects_last_admin);
    RUN_TEST(test_user_management_updates_password_and_removes_user);
    return UNITY_END();
}
