#pragma once

#include <stdint.h>

#include "TelemetrySchedule.h"

namespace radiosensors {
namespace node {

// Sleep current is too small to reveal battery sag, and the radio burst cannot
// be sampled while sendWithRetry() blocks. The reported value is the lower of
// the pre-transmission measurement and the one taken immediately after the
// previous transmission, while the cell is still recovering.
class LoadedSupplyVoltage {
  public:
    uint16_t report(const uint16_t beforeTransmission) const {
        return beforeTransmission < afterPrevious_ ? beforeTransmission : afterPrevious_;
    }

    void transmitted(const uint16_t afterTransmission) {
        afterPrevious_ = afterTransmission;
    }

    // A supply that recovered while the node stayed silent must not be
    // reported at the level of a transmission long past.
    void forget() {
        afterPrevious_ = 0xFFFFU;
    }

  private:
    uint16_t afterPrevious_ = 0xFFFFU;
};

// Keeps the radio off at or below the minimum: transmit sag there takes the
// MCU into brown-out reset before it returns the radio to standby, and the
// module then transmits until the supply is gone. Only a fresh resting
// measurement decides, because a silent node takes no measurement after a
// transmission. After a low one the gate holds without measuring, since a
// report stays due and would otherwise cost a measurement on every tick.
class SupplyGate {
  public:
    static constexpr uint32_t kRecheckMs = 60UL * 1000UL;

    explicit SupplyGate(const uint16_t minimumExclusiveMillivolts)
        : minimumExclusiveMillivolts_(minimumExclusiveMillivolts) {
    }

    bool holding(const uint32_t now) const {
        return low_ && !intervalElapsed(now, lowAt_, kRecheckMs);
    }

    // Returns whether the measurement permits transmitting.
    bool measured(const uint32_t now, const uint16_t supplyMillivolts) {
        low_ = supplyMillivolts <= minimumExclusiveMillivolts_;
        if (low_)
            lowAt_ = now;
        return !low_;
    }

  private:
    uint16_t minimumExclusiveMillivolts_;
    uint32_t lowAt_ = 0;
    bool low_ = false;
};

} // namespace node
} // namespace radiosensors
