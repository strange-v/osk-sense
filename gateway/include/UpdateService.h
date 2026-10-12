#pragma once

#include <Arduino.h>

#include "ReleaseManifest.h"

namespace gateway::update {

enum class State : uint8_t {
    Idle,
    Checking,
    UpToDate,
    Available,
    Installing,
    Restarting,
    Failed,
};

struct Status {
    State state = State::Idle;
    char availableVersion[release::kMaxVersionLength + 1]{};
    uint8_t progress = 0;
    // Constant error code of the last failed step, empty otherwise.
    const char *error = "";
    // The running image is on trial: the bootloader returns to the previous
    // one unless this boot confirms it.
    bool pendingVerify = false;
};

void begin();
// Confirms or rejects an image on trial and restarts after an install.
void loop();
// Both run in a background task; false when one is already running or the
// request does not apply.
bool startCheck();
bool startInstall();
// Reinstalls the Web UI of the running firmware's release, then restarts.
bool startUiRepair();
Status status();
const char *stateName(State state);

} // namespace gateway::update
