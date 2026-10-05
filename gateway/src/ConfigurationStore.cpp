#include "ConfigurationStore.h"

#include <Preferences.h>
#include <esp_random.h>

#include "NodeRegistryStore.h"
#include "RecoveryService.h"

namespace gateway::configuration_store {
namespace {

using namespace radiosensors::gateway_storage;

class NvsSlotStorage final : public SlotStorage {
public:
    NvsSlotStorage(const char* nameSpace, const char* slotA, const char* slotB)
        : nameSpace_(nameSpace), keys_{slotA, slotB} {}

    bool begin() { return preferences_.begin(nameSpace_, false); }

    bool exists(uint8_t slot) override {
        return slot < 2 && preferences_.getBytesLength(keys_[slot]) != 0;
    }

    bool read(uint8_t slot, uint8_t* output, size_t capacity, size_t& size) override {
        size = 0;
        if (slot >= 2 || output == nullptr) return false;
        const size_t storedSize = preferences_.getBytesLength(keys_[slot]);
        if (storedSize == 0 || storedSize > capacity) return false;
        const size_t readSize = preferences_.getBytes(keys_[slot], output, storedSize);
        if (readSize != storedSize) return false;
        size = readSize;
        return true;
    }

    bool write(uint8_t slot, const uint8_t* data, size_t size) override {
        return slot < 2 && data != nullptr &&
            preferences_.putBytes(keys_[slot], data, size) == size;
    }

private:
    const char* nameSpace_;
    const char* keys_[2];
    Preferences preferences_;
};

NvsSlotStorage settingsSlots("gateway-config", "config_a", "config_b");
NvsSlotStorage authSlots("gateway-auth", "auth_a", "auth_b");
NvsSlotStorage secretSlots("gateway-secrets", "secret_a", "secret_b");
SettingsStore settingsStore(settingsSlots);
AuthenticationStore authStore(authSlots);
SecretsStore secretStore(secretSlots);
GatewaySettings currentSettings{};
AuthenticationData currentAuthentication{};
InstallationSecrets currentSecrets{};
SemaphoreHandle_t mutex = nullptr;
bool initialized = false;

template<typename Model, typename Store>
SaveStatus saveLocked(Store& store, Model& current, const Model& value,
                      bool (*equal)(const Model&, const Model&)) {
    if (equal(current, value)) return SaveStatus::NoChange;
    if (!store.save(value)) return SaveStatus::StorageError;
    current = value;
    return SaveStatus::Ok;
}

bool bootstrapSecrets() {
    currentSecrets = defaultSecrets();
    currentSecrets.deviceSecretPresent = true;
    esp_fill_random(currentSecrets.deviceSecret, kDeviceSecretSize);
    // Give the record a usable network id from the very first boot: the
    // default struct is zero-filled, and zero would otherwise be reported by
    // /health and handed to the radio on a gateway that has not been set up.
    do currentSecrets.operationalNetworkId = static_cast<uint8_t>(esp_random());
    while (currentSecrets.operationalNetworkId == 0);

    return secretStore.save(currentSecrets);
}

}  // namespace

bool begin() {
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        Serial.println("Gateway storage mutex creation failed");
        return false;
    }
    if (!settingsSlots.begin() || !authSlots.begin() || !secretSlots.begin()) {
        Serial.println("Gateway storage NVS initialization failed");
        return false;
    }

    const LoadStatus settingsSource = settingsStore.load(currentSettings);
    const LoadStatus authSource = authStore.load(currentAuthentication);
    const LoadStatus secretSource = secretStore.load(currentSecrets);
    if (settingsSource == LoadStatus::Corrupt ||
        authSource == LoadStatus::Corrupt ||
        secretSource == LoadStatus::Corrupt) {
        Serial.println("Gateway storage contains data but no valid snapshot");
        return false;
    }
    if (secretSource == LoadStatus::Empty && !bootstrapSecrets()) {
        Serial.println("Gateway secret bootstrap failed");
        return false;
    }

    initialized = true;
    Serial.printf(
        "Gateway storage ready: settings=%lu/%s auth=%lu/%s secrets=%lu/%s\n",
        static_cast<unsigned long>(settingsStore.generation()),
        settingsSource == LoadStatus::Loaded ? "nvs" : "defaults",
        static_cast<unsigned long>(authStore.generation()),
        authSource == LoadStatus::Loaded ? "nvs" : "defaults",
        static_cast<unsigned long>(secretStore.generation()),
        secretSource == LoadStatus::Loaded ? "nvs" : "bootstrap");
    return true;
}

bool ready() { return initialized; }

GatewaySettings settings() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return defaultSettings();
    const GatewaySettings value = currentSettings;
    xSemaphoreGive(mutex);
    return value;
}

AuthenticationData authentication() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return defaultAuthentication();
    const AuthenticationData value = currentAuthentication;
    xSemaphoreGive(mutex);
    return value;
}

InstallationSecrets secrets() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
        return defaultSecrets();
    const InstallationSecrets value = currentSecrets;
    xSemaphoreGive(mutex);
    return value;
}

uint32_t settingsGeneration() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t value = settingsStore.generation();
    xSemaphoreGive(mutex);
    return value;
}

uint32_t authenticationGeneration() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t value = authStore.generation();
    xSemaphoreGive(mutex);
    return value;
}

uint32_t secretsGeneration() {
    if (!initialized || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
    const uint32_t value = secretStore.generation();
    xSemaphoreGive(mutex);
    return value;
}

SaveStatus saveSettings(const GatewaySettings& value) {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !initialized) return SaveStatus::NotInitialized;
    uint8_t encoded[kSettingsSnapshotSize]{};
    if (encodeSettings(value, 0, encoded, sizeof(encoded)) != CodecStatus::Ok)
        return SaveStatus::Invalid;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const SaveStatus result = saveLocked(settingsStore, currentSettings, value, settingsEqual);
    xSemaphoreGive(mutex);
    return result;
}

SaveStatus saveAuthentication(const AuthenticationData& value) {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !initialized) return SaveStatus::NotInitialized;
    uint8_t encoded[kAuthSnapshotSize]{};
    if (encodeAuthentication(value, 0, encoded, sizeof(encoded)) != CodecStatus::Ok)
        return SaveStatus::Invalid;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const SaveStatus result = saveLocked(
        authStore, currentAuthentication, value, authenticationEqual);
    xSemaphoreGive(mutex);
    return result;
}

SaveStatus saveSecrets(const InstallationSecrets& value) {
    recovery::Guard guard;
    if (!guard || recovery::blocked() || !initialized) return SaveStatus::NotInitialized;
    uint8_t encoded[kSecretsSnapshotSize]{};
    if (encodeSecrets(value, 0, encoded, sizeof(encoded)) != CodecStatus::Ok)
        return SaveStatus::Invalid;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool radioProfileChanged =
        value.radioConfigured != currentSecrets.radioConfigured ||
        value.operationalNetworkId != currentSecrets.operationalNetworkId;
    if (radioProfileChanged && registry_store::hasActiveNodes()) {
        xSemaphoreGive(mutex);
        return SaveStatus::LockedByActiveNodes;
    }
    const SaveStatus result = saveLocked(secretStore, currentSecrets, value, secretsEqual);
    xSemaphoreGive(mutex);
    return result;
}

}  // namespace gateway::configuration_store
