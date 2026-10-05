#pragma once

#include <Arduino.h>
#include <RadioSecurity.h>
#include <RadioProtocol.h>

namespace gateway::radio {

constexpr size_t kMaxPayloadSize = 61;

enum class Profile {
    Operational,
    Commissioning,
};

struct ReceivedFrame {
    uint8_t data[kMaxPayloadSize];
    uint8_t size;
    uint16_t senderId;
    int16_t rssi;
    bool ackRequested;
    uint32_t receivedAtMs;
    uint16_t targetId;
    uint8_t control;
    radiosensors::protocol::FrameKind kind;
    uint32_t counter;
    uint8_t salt[radiosensors::security::kSaltSize];
};

enum class State {
    Stopped,
    Starting,
    SpiInitializationFailed,
    InitializationFailed,
    VersionMismatch,
    NetworkMissing,
    TaskFailed,
    Receiving,
};

struct Snapshot {
    State state;
    uint8_t version;
    uint32_t frequencyHz;
    uint32_t bitRate;
    int8_t configuredPowerDbm;
    bool encryptionEnabled;
    uint32_t interrupts;
    uint32_t missedInterrupts;
    uint32_t moduleRestores;
    uint32_t moduleRestoreFailures;
    uint32_t frequencyFaults;
    uint32_t packets;
    uint32_t bytes;
    uint32_t emptyWakeups;
    uint32_t ackRequestsIgnored;
    uint32_t telemetryAcksSent;
    uint32_t telemetryRejectedInactive;
    uint32_t telemetryFramesQueued;
    uint32_t telemetryFramesDropped;
    uint32_t v3Frames;
    uint32_t v3TelemetryFrames;
    uint32_t failedTags;
    uint32_t replayFrames;
    uint32_t activationChallenges;
    uint32_t acksMeasured, lastAckUs, maxAckUs, acksOver40ms;
    uint32_t emptyApplicationFrames;
    uint32_t unsupportedProtocolVersions;
    uint32_t unsupportedFrameKinds;
    uint32_t rxFramesQueued;
    uint32_t rxFramesDropped;
    uint32_t commandsQueued;
    uint32_t commandsDropped;
    uint32_t commandsProcessed;
    uint32_t sessionFramesQueued;
    uint32_t sessionFramesDropped;
    uint32_t sessionFramesRejected;
    uint32_t commandResultAcksSent;
    uint32_t commandHintsSent;
    uint32_t powerTargetsSent;
    Profile profile;
    uint8_t currentNetworkId;
    uint32_t lastPacketMs;
    uint16_t lastSenderId;
    int16_t lastRssi;
};

// Returns true once the radio task runs. Initial setup supplies its network.
bool begin();
bool receive(ReceivedFrame& frame, TickType_t waitTicks = 0);
bool receiveTelemetry(ReceivedFrame& frame, TickType_t waitTicks = 0);
// Command ready and Command result frames from active nodes.
bool receiveSessionFrame(ReceivedFrame& frame, TickType_t waitTicks = 0);
bool requestProfile(Profile profile);
bool beginCommissioning();
// Switches to the operational network with hardware AES disabled.
bool applyInstallation(uint8_t networkId);
bool send(uint16_t targetId, const uint8_t* data, size_t size, bool requestAck = false);
bool sendCommandReply(const ReceivedFrame& ready, uint8_t header, const uint8_t* payload, size_t size);
bool sendThenSwitchProfile(
    uint16_t targetId,
    const uint8_t* data,
    size_t size,
    Profile profile);
State state();
const char* stateName();
const char* frequencyBandName();
const char* spiHostName();
const char* profileName();
Snapshot snapshot();
uint32_t stackFreeBytes();

}  // namespace gateway::radio
