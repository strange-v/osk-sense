#pragma once

#include <stdint.h>

namespace radiosensors {
namespace node {

// Frees an I2C bus whose target stopped mid-byte and holds SDA low: clocks
// SCL until the target releases SDA, then generates STOP. Returns whether
// both lines are high afterwards.
//
// Bus provides open-drain control of the two lines:
//   void pullSclLow();  void releaseScl();
//   void pullSdaLow();  void releaseSda();
//   bool sclHigh();     bool sdaHigh();
//   void halfPeriod();  // half an SCL period
template <typename Bus> bool recoverI2cBus(Bus &bus) {
    // A target finishes the byte it is sending in at most nine clocks: eight
    // data bits and the acknowledgement.
    static constexpr uint8_t kMaximumClocks = 9;

    bus.releaseSda();
    bus.releaseScl();
    bus.halfPeriod();
    for (uint8_t clock = 0; clock < kMaximumClocks && !bus.sdaHigh(); ++clock) {
        bus.pullSclLow();
        bus.halfPeriod();
        bus.releaseScl();
        bus.halfPeriod();
    }

    // STOP: SDA rises while SCL is high.
    bus.pullSclLow();
    bus.halfPeriod();
    bus.pullSdaLow();
    bus.halfPeriod();
    bus.releaseScl();
    bus.halfPeriod();
    bus.releaseSda();
    bus.halfPeriod();
    return bus.sclHigh() && bus.sdaHigh();
}

} // namespace node
} // namespace radiosensors
