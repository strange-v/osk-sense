#pragma once

#include <stdint.h>

namespace radiosensors {

// Feed debounced levels. A reset needs a 10-second hold, a release, and a
// second press within 5 seconds; a short press acts on release.
class ResetButton {
  public:
    enum class Action { None, ShortPress, ConfirmReset };

    static constexpr uint32_t kShortPressMs = 1000;
    static constexpr uint32_t kHoldMs = 10000;
    static constexpr uint32_t kConfirmWindowMs = 5000;

    Action update(const bool pressed, const uint32_t now) {
        if (waiting_ && static_cast<uint32_t>(now - releasedAt_) >= kConfirmWindowMs)
            waiting_ = false;
        if (pressed != pressed_) {
            pressed_ = pressed;
            if (pressed) {
                pressedAt_ = now;
                if (waiting_) {
                    waiting_ = false;
                    armed_ = false;
                    suppressRelease_ = true;
                    return Action::ConfirmReset;
                }
            } else if (suppressRelease_) {
                suppressRelease_ = false;
                return Action::None;
            } else if (armed_) {
                armed_ = false;
                waiting_ = true;
                releasedAt_ = now;
            } else if (static_cast<uint32_t>(now - pressedAt_) < kShortPressMs) {
                return Action::ShortPress;
            }
        }
        if (pressed_ && !suppressRelease_ && static_cast<uint32_t>(now - pressedAt_) >= kHoldMs)
            armed_ = true;
        return Action::None;
    }

    bool confirming() const {
        return armed_ || waiting_;
    }
    // Held long enough; the release is still pending.
    bool armed() const {
        return armed_;
    }
    // Released after the hold; the confirming press is awaited.
    bool waiting() const {
        return waiting_;
    }

  private:
    bool pressed_ = false;
    bool armed_ = false;
    bool waiting_ = false;
    bool suppressRelease_ = false;
    uint32_t pressedAt_ = 0;
    uint32_t releasedAt_ = 0;
};

} // namespace radiosensors
