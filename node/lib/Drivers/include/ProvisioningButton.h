#pragma once

#include <Arduino.h>

namespace radiosensors {
namespace node {

enum class ButtonGesture : uint8_t {
    None,
    ShortPress,
    LongPress,
};

class ProvisioningButton {
  public:
    static constexpr uint32_t kLongPressMs = 10000;

    explicit ProvisioningButton(const uint8_t pin) : pin_(pin) {
    }

    void begin();

    // Times a hold with millis() while the MCU stays awake: the RTC tick is up
    // to 32 s, far too coarse for a 10-second gesture. Input polling pauses
    // for the duration of the hold.
    ButtonGesture takeGesture();

  private:
    static void onEdge();
    bool settledAt(uint8_t level) const;

    uint8_t pin_;
    static volatile bool edgePending_;
};

} // namespace node
} // namespace radiosensors
