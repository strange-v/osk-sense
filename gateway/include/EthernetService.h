#pragma once

#include <Arduino.h>

namespace gateway::ethernet {

enum class State : uint8_t {
    Stopped,
    Starting,
    LinkDown,
    LinkUp,
    Online,
    Failed,
};

bool begin();
State state();
const char *stateName();
bool hasIp();
String ipAddress();
String macAddress();

} // namespace gateway::ethernet
