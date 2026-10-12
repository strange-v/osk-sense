#pragma once

#include <Arduino.h>

namespace gateway::ota {

enum class State : uint8_t {
    Disabled,
    WaitingForNetwork,
    Ready,
    Updating,
    Failed,
};

void begin();
void loop();
bool enabled();
State state();
const char *stateName();
uint8_t progressPercent();

} // namespace gateway::ota
