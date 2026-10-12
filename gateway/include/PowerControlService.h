#pragma once

#include <Arduino.h>

#include "TelemetryStore.h"

namespace gateway::power_control {

constexpr uint8_t kNoTarget = 0xFF;

// Feeds one accepted telemetry record to its node's controller.
void observe(const telemetry_store::Record &record);
// Applies a new policy before the node's next report.
void policyChanged(uint8_t nodeId, uint8_t policy, uint8_t ceiling);
// Lock-free for the radio task: the level to ask of a node whose frame
// carries `radioState`, when it differs from the one reported.
bool target(uint8_t nodeId, uint8_t radioState, uint8_t &level);
// The level the gateway wants, or kNoTarget before the first report.
uint8_t desiredLevel(uint8_t nodeId);
void forget(uint8_t nodeId);
void clear();

} // namespace gateway::power_control
