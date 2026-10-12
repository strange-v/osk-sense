#pragma once

#include <avr/io.h>

namespace radiosensors {
namespace node {

// Resets the MCU when the scope it guards runs longer than 8 s; the longest
// legitimate one, a commissioning attempt, takes about 4 s. The watchdog
// cannot stay on across sleep, because the 32 s tick outlasts its longest
// period, and arming it on every 250 ms tick would cost more than the sleep
// current. Guard only work that touches the radio or a sensor.
class WatchdogWindow {
  public:
    WatchdogWindow() {
        waitForSync();
        _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_8KCLK_gc);
    }

    // Waits until the watchdog is off: a disable still synchronizing when the
    // MCU enters power-down must not leave it running through the tick.
    ~WatchdogWindow() {
        waitForSync();
        _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_OFF_gc);
        waitForSync();
    }

    WatchdogWindow(const WatchdogWindow &) = delete;
    WatchdogWindow &operator=(const WatchdogWindow &) = delete;

  private:
    static void waitForSync() {
        while ((WDT.STATUS & WDT_SYNCBUSY_bm) != 0) {
        }
    }
};

} // namespace node
} // namespace radiosensors
