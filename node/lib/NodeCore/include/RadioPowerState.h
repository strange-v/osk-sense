#pragma once

#include <stdint.h>

namespace radiosensors {
namespace node {

// Consecutive reports without acknowledgement before the node raises itself
// and sends single transmissions.
constexpr uint8_t kFallbackReports = 3;
constexpr uint8_t kTelemetryAttempts = 3;

// Once the gateway looks gone, each report only probes for its return, at a
// third of the charge. An event keeps every attempt: it may be the first
// report after the gateway is back.
inline uint8_t telemetryAttempts(const uint8_t unacknowledgedReports, const bool urgent) {
    return urgent || unacknowledgedReports < kFallbackReports ? kTelemetryAttempts : 1;
}

struct PowerDecision {
    bool change;
    uint8_t level;
    bool fallback;
};

// A target from the gateway, clamped to the node's ceiling. Applying one
// also ends a fallback.
inline PowerDecision afterAcknowledged(const uint8_t level,
                                       const bool fallback,
                                       const bool hasTarget,
                                       const uint8_t target,
                                       const uint8_t ceiling) {
    if (!hasTarget)
        return PowerDecision{false, level, fallback};
    const uint8_t wanted = target > ceiling ? ceiling : target;
    if (wanted == level && !fallback)
        return PowerDecision{false, level, fallback};
    return PowerDecision{true, wanted, false};
}

// The node stops trusting its level after losing the gateway, unless it is
// already at its ceiling and has nothing left to raise.
inline PowerDecision afterUnacknowledged(const uint8_t level,
                                         const bool fallback,
                                         const uint8_t failures,
                                         const uint8_t ceiling) {
    if (failures < kFallbackReports || level >= ceiling)
        return PowerDecision{false, level, fallback};
    return PowerDecision{true, ceiling, true};
}

} // namespace node
} // namespace radiosensors
