#include "RecoveryService.h"

#include <nvs.h>

#include <atomic>

#include "BackupService.h"

namespace gateway::recovery {
namespace {

// Values 0..2 are persisted as the control marker; the others exist only in RAM.
constexpr uint8_t kIdle = 0;
constexpr uint8_t kRestoreIncomplete = 1;
constexpr uint8_t kResetRequested = 2;
constexpr uint8_t kStorageUnavailable = 3;
constexpr uint8_t kRestarting = 4;

// Kept outside the namespaces being erased, and removed only after all commits.
constexpr const char* kControl = "gateway-recover";
constexpr const char* kStores[] = {
    "gateway-config", "gateway-auth", "gateway-secrets", "node-reg", "node-cmd",
    "radio-bound",
};

SemaphoreHandle_t mutex = nullptr;
std::atomic<uint8_t> state{kStorageUnavailable};
std::atomic<uint32_t> restartAt{0};

bool writeMarker(const uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(kControl, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_u8(handle, "operation", value) == ESP_OK &&
        nvs_commit(handle) == ESP_OK;
    uint8_t actual = UINT8_MAX;
    ok = ok && nvs_get_u8(handle, "operation", &actual) == ESP_OK && actual == value;
    nvs_close(handle);
    return ok;
}

bool eraseStores() {
    for (const char* name : kStores) {
        nvs_handle_t handle;
        if (nvs_open(name, NVS_READWRITE, &handle) != ESP_OK) return false;
        const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
        if (!ok) return false;
    }
    return true;
}

}  // namespace

Guard::Guard(const TickType_t wait)
    : held_(mutex != nullptr && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE) {}

Guard::~Guard() {
    if (held_) xSemaphoreGiveRecursive(mutex);
}

void begin() {
    state.store(kStorageUnavailable);
    if (mutex == nullptr) mutex = xSemaphoreCreateRecursiveMutex();
    if (mutex == nullptr) return;

    nvs_handle_t handle;
    if (nvs_open(kControl, NVS_READWRITE, &handle) != ESP_OK) {
        Serial.println("Recovery: control namespace unavailable; storage disabled");
        return;
    }
    uint8_t value = kIdle;
    const esp_err_t result = nvs_get_u8(handle, "operation", &value);
    nvs_close(handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("Recovery: control marker unreadable; storage disabled");
        return;
    }

    if (value == kResetRequested) {
        Serial.println("Recovery: erasing installation stores for factory reset");
        const bool erased = eraseStores() && writeMarker(kIdle);
        state.store(erased ? kIdle : kResetRequested);
        Serial.println(erased
            ? "Recovery: factory reset complete"
            : "Recovery: factory reset incomplete; retrying at next boot");
    } else if (value == kRestoreIncomplete) {
        state.store(value);
        Serial.println("Recovery: backup restore was interrupted; factory reset required");
    } else if (value == kIdle) {
        state.store(value);
    } else {
        Serial.printf("Recovery: unknown control marker %u; storage disabled\n", value);
    }
}

bool blocked() { return state.load() != kIdle; }

bool eraseInstallation() { return blocked() && eraseStores(); }

const char* reason() {
    switch (state.load()) {
        case kIdle: return "none";
        case kRestoreIncomplete: return "restore_incomplete";
        case kResetRequested: return "reset_incomplete";
        case kRestarting: return "restarting";
        default: return "storage_unavailable";
    }
}

bool startRestore() {
    if (blocked()) return false;
    // A failed marker write may nevertheless have reached flash. Fail closed.
    state.store(kRestoreIncomplete);
    return writeMarker(kRestoreIncomplete);
}

bool finishRestore() {
    if (!writeMarker(kIdle)) return false;
    state.store(kRestarting);
    restartSoon();
    return true;
}

bool requestFactoryReset() {
    Guard guard(0);
    if (!guard || backup::busy()) {
        Serial.println("Factory reset refused: a backup or storage write is in progress");
        return false;
    }
    if (!writeMarker(kResetRequested)) {
        state.store(kStorageUnavailable);
        Serial.println("Factory reset failed: could not record the request in NVS");
        return false;
    }
    state.store(kResetRequested);
    Serial.println("Factory reset recorded; restarting to erase installation stores");
    Serial.flush();
    ESP.restart();
    return true;
}

void restartSoon() { restartAt.store(millis() + 1000); }

void loop() {
    const uint32_t at = restartAt.load();
    if (at != 0 && static_cast<int32_t>(millis() - at) >= 0) ESP.restart();
}

}  // namespace gateway::recovery
