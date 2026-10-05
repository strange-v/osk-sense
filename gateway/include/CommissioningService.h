#pragma once

#include <Arduino.h>

namespace gateway::commissioning {

struct Snapshot {
    uint32_t joinRequests;
    uint32_t joinAcceptsQueued;
    uint32_t joinConfirms;
    uint32_t nodesActivated;
    uint32_t joinCompletesQueued;
    uint32_t rejectedFrames;
    uint32_t storageErrors;
    uint32_t confirmTimeouts;
};

bool begin();
bool open(const uint8_t deviceUid[10], const uint8_t factoryKey[16]);
bool close();
Snapshot snapshot();
uint32_t stackFreeBytes();

}  // namespace gateway::commissioning
