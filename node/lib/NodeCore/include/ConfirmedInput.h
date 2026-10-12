#pragma once

#include <stdint.h>

#include "TelemetrySchedule.h"

namespace radiosensors {
namespace node {

enum class InputChange : uint8_t {
    None,
    Fell,
    Rose,
};

// Confirms changes of a polled contact. Each tick costs one read; the debounce
// burst runs only when that read disagrees with the confirmed state, in the
// same wake-up, so confirmation adds no tick of latency.
//
// Input provides:
//   bool readOnce();           // true when the contact is open
//   bool sample(bool& high);   // false when the contact did not stabilize
class ConfirmedInput {
  public:
    explicit ConfirmedInput(const bool initialHigh) : high_(initialHigh) {
    }

    bool high() const {
        return high_;
    }

    template <typename Input> InputChange update(Input &input) {
        if (input.readOnce() == high_)
            return InputChange::None;
        bool stable = high_;
        if (!input.sample(stable) || stable == high_)
            return InputChange::None;
        high_ = stable;
        return high_ ? InputChange::Rose : InputChange::Fell;
    }

  private:
    bool high_;
};

// Accepts a level only after it has persisted for a minimum RTC time. A magnet
// moving slowly near the pull-in distance makes a reed chatter far longer than
// the in-wake debounce burst, and each extra rise would be an extra meter
// pulse. Binary inputs must not use this: it trades latency for that
// certainty.
class MinimumPhaseFilter {
  public:
    MinimumPhaseFilter(const uint32_t minimumPhaseMs, const bool initialHigh)
        : minimumPhaseMs_(minimumPhaseMs), accepted_(initialHigh) {
    }

    InputChange update(const uint32_t now, const bool high) {
        if (high == accepted_) {
            pending_ = false;
            return InputChange::None;
        }
        if (!pending_) {
            pending_ = true;
            pendingSince_ = now;
            return InputChange::None;
        }
        if (!intervalElapsed(now, pendingSince_, minimumPhaseMs_))
            return InputChange::None;
        accepted_ = high;
        pending_ = false;
        return high ? InputChange::Rose : InputChange::Fell;
    }

  private:
    uint32_t minimumPhaseMs_;
    uint32_t pendingSince_ = 0;
    bool accepted_;
    bool pending_ = false;
};

} // namespace node
} // namespace radiosensors
