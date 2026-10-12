#pragma once

#include <stdint.h>

class AsyncWebServer;
class AsyncWebServerRequest;

namespace gateway::web_ui {

enum class State : uint8_t {
    Unavailable,
    MissingManifest,
    InvalidManifest,
    Incompatible,
    Ready,
    Updating,
};

void begin();
void addRoutes(AsyncWebServer &server);
void prepareForFilesystemUpdate();
void recoverAfterFailedFilesystemUpdate();
bool handlePageRequest(AsyncWebServerRequest *request);
State state();
const char *stateName();
const char *version();
const char *requiredFirmware();
void sendRecoveryPage(AsyncWebServerRequest *request);

} // namespace gateway::web_ui
