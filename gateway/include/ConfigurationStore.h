#pragma once

#include <GatewayStorage.h>

namespace gateway::configuration_store {

enum class SaveStatus : uint8_t {
    Ok,
    NoChange,
    Invalid,
    StorageError,
    NotInitialized,
    LockedByActiveNodes,
};

bool begin();
bool ready();

radiosensors::gateway_storage::GatewaySettings settings();
radiosensors::gateway_storage::AuthenticationData authentication();
radiosensors::gateway_storage::InstallationSecrets secrets();

uint32_t settingsGeneration();
uint32_t authenticationGeneration();
uint32_t secretsGeneration();

SaveStatus saveSettings(const radiosensors::gateway_storage::GatewaySettings &value);
SaveStatus saveAuthentication(const radiosensors::gateway_storage::AuthenticationData &value);
SaveStatus saveSecrets(const radiosensors::gateway_storage::InstallationSecrets &value);

} // namespace gateway::configuration_store
