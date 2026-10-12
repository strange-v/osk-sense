#pragma once

#include "TelemetryStore.h"

namespace gateway::web_server {

void begin();
void loop();
void publishTelemetry(const telemetry_store::Record &record);

} // namespace gateway::web_server
