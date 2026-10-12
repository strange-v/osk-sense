#include "PowerControlService.h"

#include <NodeRegistry.h>
#include <RadioPowerControl.h>
#include <TelemetryFrames.h>

#include <atomic>

#include "NodeRegistryStore.h"

namespace gateway::power_control {
namespace {

namespace control = radiosensors::radio_power;
namespace protocol = radiosensors::protocol;

constexpr uint8_t kFirstNodeId = radiosensors::registry::kFirstNodeId;
constexpr uint8_t kLastNodeId = radiosensors::registry::kLastNodeId;

// Controller state belongs to the main loop and the management handlers;
// the radio task reads only the published atomics.
control::ControlState states[kLastNodeId + 1]{};
portMUX_TYPE statesMux = portMUX_INITIALIZER_UNLOCKED;
// Wanted level + 1, zero while there is none.
std::atomic<uint8_t> desired[kLastNodeId + 1]{};
// Set once the controller has seen the node's current fallback. Until then
// an acknowledgement must not carry a target computed before the fallback,
// or automatic control would send the node straight back to the level that
// failed.
std::atomic<bool> fallbackObserved[kLastNodeId + 1]{};

bool validNode(const uint8_t nodeId) {
    return nodeId >= kFirstNodeId && nodeId <= kLastNodeId;
}

void publish(const uint8_t nodeId, const uint8_t level) {
    desired[nodeId].store(level == control::kNoLevel ? 0 : static_cast<uint8_t>(level + 1U),
                          std::memory_order_release);
}

} // namespace

void observe(const telemetry_store::Record &record) {
    if (!validNode(record.nodeId))
        return;
    protocol::TelemetryView view{};
    if (protocol::decodeTelemetry(record.data, record.size, view) !=
        protocol::TelemetryCodecStatus::Ok) {
        return;
    }
    uint8_t ceiling = 0;
    uint8_t policy = 0;
    if (!registry_store::radioPolicy(record.nodeId, ceiling, policy))
        return;
    const control::Report report{protocol::radioPowerLevel(view.radioState),
                                 (view.radioState & protocol::kRadioFallback) != 0,
                                 record.rssi};
    portENTER_CRITICAL(&statesMux);
    const uint8_t level = control::observe(states[record.nodeId], policy, ceiling, report);
    portEXIT_CRITICAL(&statesMux);
    publish(record.nodeId, level);
    fallbackObserved[record.nodeId].store(report.fallback, std::memory_order_release);
}

void policyChanged(const uint8_t nodeId, const uint8_t policy, const uint8_t ceiling) {
    if (!validNode(nodeId))
        return;
    portENTER_CRITICAL(&statesMux);
    const uint8_t level = control::changePolicy(states[nodeId], policy, ceiling);
    portEXIT_CRITICAL(&statesMux);
    publish(nodeId, level);
}

bool target(const uint8_t nodeId, const uint8_t radioState, uint8_t &level) {
    if (!validNode(nodeId))
        return false;
    if ((radioState & protocol::kRadioFallback) != 0 &&
        !fallbackObserved[nodeId].load(std::memory_order_acquire)) {
        return false;
    }
    const uint8_t stored = desired[nodeId].load(std::memory_order_acquire);
    if (stored == 0)
        return false;
    level = static_cast<uint8_t>(stored - 1U);
    return level != protocol::radioPowerLevel(radioState);
}

uint8_t desiredLevel(const uint8_t nodeId) {
    if (!validNode(nodeId))
        return kNoTarget;
    const uint8_t stored = desired[nodeId].load(std::memory_order_acquire);
    return stored == 0 ? kNoTarget : static_cast<uint8_t>(stored - 1U);
}

void forget(const uint8_t nodeId) {
    if (!validNode(nodeId))
        return;
    portENTER_CRITICAL(&statesMux);
    states[nodeId] = control::ControlState{};
    portEXIT_CRITICAL(&statesMux);
    desired[nodeId].store(0, std::memory_order_release);
    fallbackObserved[nodeId].store(false, std::memory_order_release);
}

void clear() {
    for (uint8_t nodeId = kFirstNodeId; nodeId <= kLastNodeId; ++nodeId)
        forget(nodeId);
}

} // namespace gateway::power_control
