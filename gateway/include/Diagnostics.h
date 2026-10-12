#pragma once

#include <Arduino.h>

namespace gateway::diagnostics {

bool beginWatchdog();
void feedWatchdog();
const char *resetReason();

} // namespace gateway::diagnostics
