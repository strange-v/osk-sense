#pragma once

#include <stdint.h>

#include "RadioProtocol.h"

namespace radiosensors {
namespace radio_power {

// Registry encoding of a node's policy: automatic, or a fixed level + 1.
constexpr uint8_t kPolicyAuto = 0;

constexpr uint8_t fixedPolicy(const uint8_t level) {
    return static_cast<uint8_t>(level + 1U);
}

constexpr bool isFixedPolicy(const uint8_t policy) {
    return policy != kPolicyAuto;
}

constexpr uint8_t fixedLevel(const uint8_t policy) {
    return static_cast<uint8_t>(policy - 1U);
}

constexpr bool validPolicy(const uint8_t policy) {
    return policy <= protocol::kMaxRadioPowerLevel + 1U;
}

// Automatic control keeps the averaged uplink RSSI inside this window. A step
// assumes roughly one dB per level until the level table is measured.
constexpr int16_t kTargetRssiLow = -85;
constexpr int16_t kTargetRssiHigh = -75;
constexpr int16_t kTargetRssiCenter = -80;
constexpr uint8_t kReportsPerDecision = 3;
constexpr uint8_t kMaxStepDown = 3;
constexpr uint8_t kMaxStepUp = 6;
// After a fallback, automatic control stays this far above the level that
// failed, so it does not walk the node back into the same loss.
constexpr uint8_t kFallbackMargin = 3;

struct Report {
    uint8_t level;
    bool fallback;
    int16_t rssi;
};

struct ControlState {
    bool known;
    uint8_t reportedLevel;
    bool fallback;
    uint8_t reports;
    int32_t rssiSum;
    uint8_t floor;
    uint8_t desired;
};

constexpr uint8_t kNoLevel = 0xFF;

inline uint8_t minLevel(const uint8_t left, const uint8_t right) {
    return left < right ? left : right;
}

// A new policy takes effect before the node's next report. Automatic control
// starts a new average from the level last reported; before any report it
// has nothing to ask for.
inline uint8_t changePolicy(ControlState &state, const uint8_t policy, const uint8_t ceiling) {
    state.reports = 0;
    state.rssiSum = 0;
    if (isFixedPolicy(policy))
        state.desired = minLevel(fixedLevel(policy), ceiling);
    else if (state.known)
        state.desired = minLevel(state.reportedLevel, ceiling);
    else
        return kNoLevel;
    return state.desired;
}

// Records one report and returns the level the gateway wants next.
inline uint8_t
observe(ControlState &state, const uint8_t policy, const uint8_t ceiling, const Report &report) {
    if (report.fallback && state.known && !state.fallback) {
        state.floor =
            minLevel(ceiling, static_cast<uint8_t>(state.reportedLevel + kFallbackMargin));
    }
    if (!state.known || report.level != state.reportedLevel) {
        state.reports = 0;
        state.rssiSum = 0;
    }
    // While a newly reported level is averaged, keep the level already
    // wanted: a node that restarted at its ceiling goes straight back, and a
    // target whose acknowledgement was lost is not withdrawn. A fallback
    // report is taken at its word.
    const bool keepDesired = state.known && !report.fallback;
    state.known = true;
    state.reportedLevel = report.level;
    state.fallback = report.fallback;
    if (state.reports < UINT8_MAX) {
        ++state.reports;
        state.rssiSum += report.rssi;
    }

    uint8_t desired = keepDesired ? state.desired : report.level;
    // A fixed level is the user's decision and is requested again after
    // every fallback; a level too low for the link shows as repeated
    // fallbacks rather than being dropped silently.
    if (isFixedPolicy(policy)) {
        desired = fixedLevel(policy);
    } else if (state.reports >= kReportsPerDecision) {
        const int32_t average = state.rssiSum / state.reports;
        if (average > kTargetRssiHigh) {
            const int32_t excess = average - kTargetRssiCenter;
            const uint8_t step =
                excess > kMaxStepDown ? kMaxStepDown : static_cast<uint8_t>(excess);
            desired = report.level > step ? static_cast<uint8_t>(report.level - step) : 0;
        } else if (average < kTargetRssiLow) {
            const int32_t deficit = kTargetRssiCenter - average;
            const uint8_t step = deficit > kMaxStepUp ? kMaxStepUp : static_cast<uint8_t>(deficit);
            desired = static_cast<uint8_t>(report.level + step);
        }
        if (desired < state.floor)
            desired = state.floor;
    }
    state.desired = minLevel(desired, ceiling);
    return state.desired;
}

} // namespace radio_power
} // namespace radiosensors
