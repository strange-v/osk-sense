#pragma once

#include <Arduino.h>

namespace gateway::time_service {

enum class State : uint8_t {
    WaitingForNetwork,
    Disabled,
    Synchronizing,
    Synchronized,
};

void begin();
void loop();
State state();
const char *stateName();
uint64_t unixTimeMs();
uint64_t lastSyncUnixMs();

} // namespace gateway::time_service
