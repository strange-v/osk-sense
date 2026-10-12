#pragma once

#include <Arduino.h>

namespace radiosensors {
namespace node {

class PolledReedInput {
  public:
    explicit PolledReedInput(pin_size_t pin) : pin_(pin) {
    }

    // One read with the pull-up enabled only for the read. Returns true when
    // the contact is open.
    bool readOnce();

    // Returns false when the input did not stabilize within the bounded sample
    // window. rawHigh is true when the contact is open.
    bool sample(bool &rawHigh);
    void sleep();

  private:
    static constexpr uint8_t kStableSamples = 5;
    static constexpr uint8_t kMaximumSamples = 20;
    static constexpr uint16_t kSettleMicroseconds = 100;
    static constexpr uint8_t kSampleSpacingMilliseconds = 1;

    pin_size_t pin_;
};

} // namespace node
} // namespace radiosensors
