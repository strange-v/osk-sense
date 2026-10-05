#include "BackupService.h"

#include <RegistryPersistence.h>
#include <nvs.h>

#include <atomic>
#include <memory>
#include <new>

#include "ConfigurationStore.h"
#include "GatewayStatus.h"
#include "NodeRegistryStore.h"
#include "OtaService.h"
#include "RecoveryService.h"
#include "TimeService.h"

namespace gateway::backup {
namespace {

using namespace radiosensors;

// Every snapshot fits: the registry is the largest encoding.
constexpr size_t kBufferSize = registry::kMaxRegistrySnapshotSize;

std::atomic<bool> processing{false};

bool writeVerified(
    const char* space, const char* key, const uint8_t* bytes, const size_t size) {
    nvs_handle_t handle;
    if (nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_blob(handle, key, bytes, size) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    std::unique_ptr<uint8_t[]> readback(new (std::nothrow) uint8_t[size]);
    size_t length = size;
    ok = ok && readback &&
        nvs_get_blob(handle, key, readback.get(), &length) == ESP_OK &&
        length == size && memcmp(bytes, readback.get(), size) == 0;
    if (readback) wipe(readback.get(), size);
    nvs_close(handle);
    return ok;
}

}  // namespace

Operation::Operation() : held_(!processing.exchange(true)) {}

Operation::~Operation() {
    if (held_) processing.store(false);
}

bool busy() { return processing.load(); }

bool capture(Snapshot& snapshot) {
    recovery::Guard guard(0);
    if (!guard || recovery::blocked() || !configuration_store::ready() ||
        status::pairingActive() || ota::state() == ota::State::Updating) {
        return false;
    }
    std::unique_ptr<registry_store::Snapshot> registry(
        new (std::nothrow) registry_store::Snapshot{});
    if (!registry || !registry_store::snapshot(*registry)) return false;
    snapshot.settings = configuration_store::settings();
    snapshot.secrets = configuration_store::secrets();
    snapshot.createdAt = time_service::unixTimeMs();
    return snapshot.nodes.restore(registry->records, registry->count);
}

bool cleanGateway() {
    return !recovery::blocked() && configuration_store::ready() &&
        configuration_store::authentication().userCount == 0 &&
        !configuration_store::secrets().installationKeyPresent &&
        registry_store::recordCount() == 0;
}

bool restore(
    const Snapshot& snapshot, const gateway_storage::AuthenticationData& admin) {
    recovery::Guard guard;
    if (!guard || !cleanGateway() || !status::setupActive() ||
        status::pairingActive() || ota::state() == ota::State::Updating) {
        return false;
    }
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kBufferSize]);
    if (!buffer) return false;
    uint8_t* const bytes = buffer.get();

    // Validate each durable encoding before the marker or any installation write.
    size_t registrySize = 0;
    bool ok = snapshot.secrets.installationKeyPresent &&
        snapshot.secrets.deviceSecretPresent &&
        gateway_storage::encodeSettings(snapshot.settings, 1, bytes, kBufferSize) ==
            gateway_storage::CodecStatus::Ok &&
        gateway_storage::encodeSecrets(snapshot.secrets, 2, bytes, kBufferSize) ==
            gateway_storage::CodecStatus::Ok &&
        gateway_storage::encodeAuthentication(admin, 1, bytes, kBufferSize) ==
            gateway_storage::CodecStatus::Ok &&
        registry::encodeRegistrySnapshot(
            snapshot.nodes, 1, bytes, kBufferSize, registrySize) ==
            registry::SnapshotStatus::Ok;
    if (!ok) {
        wipe(bytes, kBufferSize);
        Serial.println("Backup restore rejected: snapshot does not encode");
        return false;
    }
    Serial.printf(
        "Backup restore: writing %u nodes and installation secrets\n",
        static_cast<unsigned>(snapshot.nodes.size()));
    ok = recovery::startRestore();
    if (ok) ok = recovery::eraseInstallation();
    if (ok) ok = writeVerified("node-reg", "registry", bytes, registrySize);
    if (ok) {
        gateway_storage::encodeSettings(snapshot.settings, 1, bytes, kBufferSize);
        ok = writeVerified(
            "gateway-config", "config_a", bytes, gateway_storage::kSettingsSnapshotSize);
    }
    if (ok) {
        // Both slots replace the bootstrapped device secret; no old generation can win.
        gateway_storage::encodeSecrets(snapshot.secrets, 2, bytes, kBufferSize);
        ok = writeVerified(
                 "gateway-secrets", "secret_a", bytes,
                 gateway_storage::kSecretsSnapshotSize) &&
            writeVerified(
                "gateway-secrets", "secret_b", bytes,
                gateway_storage::kSecretsSnapshotSize);
    }
    if (ok) {
        gateway_storage::encodeAuthentication(admin, 1, bytes, kBufferSize);
        ok = writeVerified(
            "gateway-auth", "auth_a", bytes, gateway_storage::kAuthSnapshotSize);
    }
    wipe(bytes, kBufferSize);
    if (!ok || !recovery::finishRestore()) {
        Serial.println("Backup restore failed while writing; factory reset required");
        return false;
    }
    Serial.println("Backup restore complete; restarting");
    return true;
}

}  // namespace gateway::backup
