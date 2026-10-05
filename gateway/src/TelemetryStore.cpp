#include "TelemetryStore.h"

#include <NodeRegistry.h>

#include "NodeRegistryStore.h"
#include "TimeService.h"

namespace gateway::telemetry_store {
namespace {

constexpr uint8_t kFirstNodeId = radiosensors::registry::kFirstNodeId;
constexpr uint8_t kLastNodeId = radiosensors::registry::kLastNodeId;

Record records[kLastNodeId + 1]{};
bool present[kLastNodeId + 1]{};
SemaphoreHandle_t mutex = nullptr;
uint8_t nodesSeen = 0;
uint32_t updates = 0;
uint8_t lastNodeId = 0;

}  // namespace

bool begin() {
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        Serial.println("Telemetry store mutex creation failed");
        return false;
    }
    Serial.println("Telemetry store ready");
    return true;
}

bool accept(const radio::ReceivedFrame& frame) {
    if (mutex == nullptr || frame.senderId < kFirstNodeId ||
        frame.senderId > kLastNodeId || frame.size > radio::kMaxPayloadSize) {
        return false;
    }

    uint16_t profileId = 0;
    if (!registry_store::activeFrameIdentity(
            static_cast<uint8_t>(frame.senderId), frame.salt, nullptr, profileId)) {
        return false;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const uint8_t nodeId = static_cast<uint8_t>(frame.senderId);
    Record& record = records[nodeId];
    if (!present[nodeId]) {
        present[nodeId] = true;
        ++nodesSeen;
    }
    record.nodeId = nodeId;
    record.profileId = profileId;
    record.size = frame.size;
    record.rssi = frame.rssi;
    record.receivedAtUnixMs = time_service::unixTimeMs();
    record.sequence = ++updates;
    memcpy(record.data, frame.data, frame.size);
    lastNodeId = nodeId;
    xSemaphoreGive(mutex);
    return true;
}

bool find(const uint8_t nodeId, Record& record) {
    if (mutex == nullptr || nodeId < kFirstNodeId || nodeId > kLastNodeId) {
        return false;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool found = present[nodeId];
    if (found) record = records[nodeId];
    xSemaphoreGive(mutex);
    return found;
}

void clear() {
    if (mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    for (uint8_t nodeId = kFirstNodeId; nodeId <= kLastNodeId; ++nodeId) {
        present[nodeId] = false;
        records[nodeId] = Record{};
    }
    nodesSeen = 0;
    lastNodeId = 0;
    xSemaphoreGive(mutex);
}

void erase(const uint8_t nodeId) {
    if (mutex == nullptr || nodeId < kFirstNodeId || nodeId > kLastNodeId ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    if (present[nodeId]) {
        present[nodeId] = false;
        records[nodeId] = Record{};
        if (nodesSeen != 0) --nodesSeen;
        if (lastNodeId == nodeId) {
            lastNodeId = 0;
            uint32_t newestSequence = 0;
            for (uint8_t candidate = kFirstNodeId;
                 candidate <= kLastNodeId; ++candidate) {
                if (present[candidate] &&
                    records[candidate].sequence > newestSequence) {
                    newestSequence = records[candidate].sequence;
                    lastNodeId = candidate;
                }
            }
        }
    }
    xSemaphoreGive(mutex);
}

Snapshot snapshot() {
    Snapshot result{};
    if (mutex == nullptr || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return result;
    }
    result.nodesSeen = nodesSeen;
    result.updates = updates;
    result.hasLast = lastNodeId != 0 && present[lastNodeId];
    if (result.hasLast) result.last = records[lastNodeId];
    xSemaphoreGive(mutex);
    return result;
}

}  // namespace gateway::telemetry_store
