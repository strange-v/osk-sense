#pragma once

#include <GatewayStorage.h>
#include <NodeRegistry.h>

#include <string>

namespace gateway::backup {

// Includes exact pairing snapshots and keys for all 64 nodes.
constexpr size_t kMaxPayload = 34816;
constexpr size_t kHeaderSize = 44;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxFile = kHeaderSize + kMaxPayload + kTagSize;
constexpr uint32_t kIterations = 100000;

void wipe(void* data, size_t size);

struct Snapshot {
    radiosensors::gateway_storage::GatewaySettings settings{};
    radiosensors::gateway_storage::InstallationSecrets secrets{};
    radiosensors::registry::NodeRegistry nodes;
    uint64_t createdAt = 0;

    ~Snapshot() {
        wipe(&settings, sizeof(settings));
        wipe(&secrets, sizeof(secrets));
        wipe(&nodes, sizeof(nodes));
    }
};

bool encode(const Snapshot& snapshot, std::string& json);
bool decode(const char* json, size_t size, Snapshot& snapshot);
void gatewayId(const Snapshot& snapshot, char output[33]);
bool encrypt(
    const Snapshot& snapshot, const char* password, size_t length,
    std::string& file);
bool decrypt(
    const uint8_t* file, size_t size, const char* password, size_t length,
    Snapshot& snapshot);
bool validPassword(const char* password, size_t length);

}  // namespace gateway::backup
