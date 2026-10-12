#pragma once

#include <Arduino.h>

#include "RadioService.h"

namespace gateway::telemetry_store {

struct Record {
    uint8_t nodeId;
    uint16_t profileId;
    uint8_t data[radio::kMaxPayloadSize];
    uint8_t size;
    int16_t rssi;
    uint64_t receivedAtUnixMs;
    uint32_t sequence;
};

struct Snapshot {
    uint8_t nodesSeen;
    uint32_t updates;
    bool hasLast;
    Record last;
};

bool begin();
bool accept(const radio::ReceivedFrame &frame);
bool find(uint8_t nodeId, Record &record);
void erase(uint8_t nodeId);
// Forgets every cached reading. Pairs with the registry clear performed by the
// radio network reset.
void clear();
Snapshot snapshot();

} // namespace gateway::telemetry_store
