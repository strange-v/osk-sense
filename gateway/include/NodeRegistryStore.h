#pragma once

#include <NodeRegistry.h>

enum class RegistryCommitStatus {
    Ok,
    NoChange,
    StorageError,
    NotInitialized,
};

namespace gateway::registry_store {

struct Snapshot {
    uint32_t generation;
    size_t count;
    radiosensors::registry::NodeRecord records[radiosensors::registry::kMaxNodes];
};

bool begin();
uint32_t generation();
size_t recordCount();
bool isActiveNode(uint8_t nodeId);
bool hasActiveNodes();
bool activeProfileId(uint8_t nodeId, uint16_t& profileId);
bool activeIdentity(uint8_t nodeId, uint8_t* deviceUid, uint16_t& profileId);
// Transmit power ceiling and policy of an active node.
bool radioPolicy(uint8_t nodeId, uint8_t& maxPowerLevel, uint8_t& powerPolicy);
bool snapshot(Snapshot& value);
// V3 authenticated pairing; the caller owns the UID-specific pairing window.
radiosensors::security::pairing::Status pairingRequestAndSave(
    const osk::crypto::Cmac& factoryMac, const uint8_t* expectedUid,
    radiosensors::security::Transport transport, const uint8_t* wire, size_t size,
    uint8_t networkId, radiosensors::security::Transport acceptTransport,
    uint8_t* output, size_t capacity);
radiosensors::security::pairing::Status pairingConfirmAndSave(
    uint8_t nodeId, radiosensors::security::Transport transport,
    const uint8_t* wire, size_t size, uint8_t* output, size_t capacity);
bool activeSecurity(uint8_t nodeId, radiosensors::security::Keys& keys, uint8_t& replaySlot);
RegistryCommitStatus reserveAndSave(
    const radiosensors::protocol::JoinRequest& request,
    radiosensors::registry::ReserveResult& result);
RegistryCommitStatus confirmAndSave(
    const uint8_t* deviceUid,
    uint8_t nodeId,
    uint32_t nonce,
    radiosensors::registry::ConfirmStatus& result);
RegistryCommitStatus renameAndSave(
    uint8_t nodeId,
    const char* displayName,
    size_t length,
    radiosensors::registry::RenameStatus& result);
RegistryCommitStatus setPowerPolicyAndSave(
    uint8_t nodeId,
    uint8_t policy,
    radiosensors::registry::PowerPolicyStatus& result);
RegistryCommitStatus updateInfoAndSave(
    uint8_t nodeId,
    const uint8_t* deviceUid,
    const radiosensors::protocol::NodeInfo& info,
    radiosensors::registry::InfoStatus& result);
RegistryCommitStatus removeAndSave(uint8_t nodeId, bool& removed);
// Drops every record in a single commit. Used by the radio network reset,
// where removing nodes one at a time would mean one flash write each and a
// half-emptied registry if any of them failed.
RegistryCommitStatus clearAndSave(size_t& removed);

}  // namespace gateway::registry_store
