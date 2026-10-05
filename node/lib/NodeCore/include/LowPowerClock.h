#pragma once

#include <stdint.h>

#if !defined(NODE_TICK_MS)
#error "NODE_TICK_MS must be defined by the build environment"
#endif

namespace radiosensors {
namespace node {

class LowPowerClock {
public:
    // One ATtiny3224 RTC PIT period. Periodic nodes use 32 s so they do not
    // pay for wake-ups they never use; polled inputs need 250 ms.
    static constexpr uint32_t kTickMs = NODE_TICK_MS;

    // Time is deliberately tracked outside millis(), whose timer stops in
    // power-down sleep.
    void begin();
    uint32_t nowMs() const;
    void sleepUntilInterrupt() const;
};

}  // namespace node
}  // namespace radiosensors
