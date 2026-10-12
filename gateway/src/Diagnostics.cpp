#include "Diagnostics.h"

#include <esp32-hal.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace gateway::diagnostics {
namespace {

bool watchdogRegistered = false;

} // namespace

bool beginWatchdog() {
    // Arduino initializes the global TWDT before setup(). Reconfiguring it here
    // corrupts ownership of its internal synchronization objects on ESP-IDF 5.5.
    // Subscribe only the Arduino loop task through the framework-supported API.
    enableLoopWDT();
    watchdogRegistered = esp_task_wdt_status(nullptr) == ESP_OK;
    return watchdogRegistered;
}

void feedWatchdog() {
    if (watchdogRegistered)
        esp_task_wdt_reset();
}

const char *resetReason() {
    switch (esp_reset_reason()) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        return "other";
    }
}

} // namespace gateway::diagnostics
