#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CommandSessionFrames.h"
#include "JoinRequest.h"
#include "PairingTransaction.h"

namespace radiosensors {
namespace registry {

constexpr size_t kMaxNodes = 64;
constexpr uint8_t kFirstNodeId = 1;
constexpr uint8_t kLastNodeId = 99;
constexpr uint8_t kUnprovisionedNodeId = 0;
constexpr uint8_t kGatewayNodeId = 100;
constexpr uint8_t kBroadcastNodeId = 255;
constexpr size_t kNodeDisplayNameSize = 48;

enum class NodeState : uint8_t {
    Pending = 1,
    Active = 2,
    Disabled = 3,
};

struct NodeRecord {
    uint8_t deviceUid[protocol::kDeviceUidSize];
    uint8_t nodeId;
    uint16_t profileId;
    protocol::FirmwareVersion firmware;
    NodeState state;
    uint32_t requestNonce;
    uint8_t displayNameLength;
    char displayName[kNodeDisplayNameSize];
    // Transmit power ceiling from Join request, and the radio_power policy.
    uint8_t maxPowerLevel;
    uint8_t powerPolicy;
    uint8_t replaySlot = 255;
    uint8_t pairing[security::pairing::kSnapshotSize]{};
};

enum class PowerPolicyStatus : uint8_t {
    Updated,
    NoChange,
    NotFound,
    InvalidPolicy,
};

enum class RenameStatus : uint8_t {
    Renamed,
    NoChange,
    NotFound,
    InvalidName,
};

enum class InfoStatus : uint8_t {
    Updated,
    ProfileChanged,
    NoChange,
    NotFound,
};

enum class ReserveStatus : uint8_t {
    Created,
    ExistingPendingUpdated,
    ExistingActive,
    ExistingDisabled,
    InvalidProfileId,
    ProfileConflict,
    Full,
    SecureTransaction,
};

struct ReserveResult {
    ReserveStatus status;
    uint8_t nodeId;
};

enum class ConfirmStatus : uint8_t {
    Confirmed,
    AlreadyActive,
    NotFound,
    NotPending,
    IdentityMismatch,
    NonceMismatch,
};

class NodeRegistry {
public:
    NodeRegistry();

    size_t size() const;
    const NodeRecord* records() const;
    const NodeRecord* findByUid(const uint8_t* deviceUid) const;
    const NodeRecord* findByNodeId(uint8_t nodeId) const;

    ReserveResult reserve(const protocol::JoinRequest& request);
    ConfirmStatus confirm(
        const uint8_t* deviceUid,
        uint8_t nodeId,
        uint32_t requestNonce);
    bool disable(uint8_t nodeId);
    bool remove(uint8_t nodeId);
    RenameStatus rename(uint8_t nodeId, const char* displayName, size_t length);
    // A fixed level must be within the node's ceiling.
    PowerPolicyStatus setPowerPolicy(uint8_t nodeId, uint8_t policy);
    // Replaces the identity an active node reported in ReadInfo. A fixed
    // power level above a lowered ceiling returns to automatic control.
    InfoStatus updateInfo(
        uint8_t nodeId, const uint8_t* deviceUid, const protocol::NodeInfo& info);

    bool restore(const NodeRecord* records, size_t count);
    // Commits authenticated transaction metadata together with its exact reply and keys.
    bool setPairing(const security::pairing::Record& pairing, uint32_t generation,
                    uint8_t replaySlot);

private:
    NodeRecord records_[kMaxNodes];
    size_t count_;

    static bool uidEquals(const uint8_t* left, const uint8_t* right);
    static bool firmwareEquals(
        const protocol::FirmwareVersion& left,
        const protocol::FirmwareVersion& right);
    uint8_t allocateNodeId() const;
    NodeRecord* findMutableByUid(const uint8_t* deviceUid);
    NodeRecord* findMutableByNodeId(uint8_t nodeId);
};

bool validDisplayName(const char* value, size_t length);

}  // namespace registry
}  // namespace radiosensors
