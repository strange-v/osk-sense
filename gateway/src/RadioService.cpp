#include "RadioService.h"

#include <RFM69.h>
#include <RFM69registers.h>
#include <RadioSecurityFrames.h>
#include <JoinRequest.h>
#include <SPI.h>
#include <TelemetryFrames.h>

#include <atomic>

#include "CommandService.h"
#include "PowerControlService.h"
#include "RadioConfig.h"
#include "NodeRegistryStore.h"
#include "ConfigurationStore.h"

namespace gateway::radio {
namespace {


constexpr uint8_t kExpectedVersion = 0x24;
constexpr uint32_t kTaskStackSize = 8192;
// Radio FIFO service is the gateway's highest-priority application work.
// Keep it above AsyncTCP (priority 10) while leaving the upper FreeRTOS
// priorities available to the ESP-IDF system tasks.
constexpr UBaseType_t kTaskPriority = 11;
constexpr BaseType_t kTaskCore = 1;
constexpr UBaseType_t kRxQueueDepth = 16;
constexpr UBaseType_t kSessionQueueDepth = 8;
constexpr UBaseType_t kCommandQueueDepth = 8;
constexpr uint8_t kInitializationAttempts = 3;
constexpr uint32_t kInitializationRecoveryDelayMs = 25;
constexpr TickType_t kCommandCompletionTimeout = pdMS_TO_TICKS(3000);
// How long the radio task waits for an interrupt before checking DIO0 itself.
constexpr TickType_t kMissedInterruptCheck = pdMS_TO_TICKS(5000);
constexpr uint32_t kRegisterCheckIntervalMs = 5000;
// Longest wait for ModeReady before OpMode is trusted; the switch to RX takes
// well under a millisecond.
constexpr uint32_t kModeReadyTimeoutMs = 5;
constexpr uint8_t kOpModeModeMask = 0x1C;

// RegFrf as RFM69::initialize() writes it for the configured band.
constexpr uint8_t kExpectedFrf[] = {
    config::frequencyBand == RF69_315MHZ ? RF_FRFMSB_315
        : config::frequencyBand == RF69_433MHZ ? RF_FRFMSB_433_92
        : config::frequencyBand == RF69_868MHZ ? RF_FRFMSB_868
        : RF_FRFMSB_915,
    config::frequencyBand == RF69_315MHZ ? RF_FRFMID_315
        : config::frequencyBand == RF69_433MHZ ? RF_FRFMID_433_92
        : config::frequencyBand == RF69_868MHZ ? RF_FRFMID_868
        : RF_FRFMID_915,
    config::frequencyBand == RF69_315MHZ ? RF_FRFLSB_315
        : config::frequencyBand == RF69_433MHZ ? RF_FRFLSB_433_92
        : config::frequencyBand == RF69_868MHZ ? RF_FRFLSB_868
        : RF_FRFLSB_915,
};

// Configuration registers only the radio task writes. A module that reset
// itself or lost a setting keeps DIO0 low and never interrupts, so only
// reading them back shows it. AES remains disabled in every profile.
// OpMode is not here: the
// sequencer passes through FS on its way to RX, so it is checked for RX once
// the module reports ModeReady.
constexpr uint8_t kCheckedRegisters[] = {
    REG_DATAMODUL, REG_BITRATEMSB, REG_BITRATELSB,
    REG_FDEVMSB, REG_FDEVLSB, REG_FRFMSB, REG_FRFMID, REG_FRFLSB,
    REG_PALEVEL, REG_OCP, REG_RXBW, REG_DIOMAPPING1, REG_DIOMAPPING2,
    REG_RSSITHRESH, REG_SYNCCONFIG, REG_SYNCVALUE1, REG_SYNCVALUE2,
    REG_PACKETCONFIG1, REG_PAYLOADLENGTH, REG_FIFOTHRESH, REG_PACKETCONFIG2,
    REG_TESTPA1, REG_TESTPA2, REG_TESTDAGC,
};

// Exposes the library's pending-interrupt flag, so a missed DIO0 edge can be
// serviced as if the interrupt had fired.
class GatewayRfm69 : public RFM69 {
public:
    using RFM69::RFM69;
    void markInterrupt() { _haveData = true; }
    uint8_t control = 0;
protected:
    void interruptHook(uint8_t value) override { control = value; }
};

SPIClass radioSpi(config::spiHost);
GatewayRfm69 rfm69(
    config::chipSelect,
    config::interrupt,
    config::highPower,
    &radioSpi);

std::atomic<State> currentState{State::Stopped};
std::atomic<Profile> currentProfile{Profile::Operational};
std::atomic<uint8_t> currentNetworkId{0};
uint8_t operationalNetworkId = 0;
uint8_t commissioningNetworkId = 0;
// Read by web and service tasks; after begin() only the radio task sets it.
std::atomic<bool> operationalEnabled{false};
std::atomic<bool> commissioningEnabled{false};
TaskHandle_t radioTaskHandle = nullptr;
QueueHandle_t interruptQueue = nullptr;
QueueHandle_t receivedFrameQueue = nullptr;
QueueHandle_t telemetryFrameQueue = nullptr;
QueueHandle_t sessionFrameQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t commandCompletionQueue = nullptr;
SemaphoreHandle_t synchronousCommandMutex = nullptr;
QueueSetHandle_t radioQueueSet = nullptr;

enum class CommandKind : uint8_t { SetProfile, BeginCommissioning, ApplyInstallation, Send, SessionReply };

struct RadioCommand {
    CommandKind kind;
    Profile profile;
    uint16_t targetId;
    uint8_t data[kMaxPayloadSize];
    uint32_t counter;
    uint8_t salt[radiosensors::security::kSaltSize];
    uint8_t header;
    uint8_t networkId;
    uint8_t size;
    bool requestAck;
    bool switchAfterSend;
    bool reportCompletion;
    uint32_t completionId;
};

std::atomic<uint32_t> nextCompletionId{1};

struct TaskStats {
    uint32_t interrupts = 0;
    uint32_t missedInterrupts = 0;
    uint32_t moduleRestores = 0;
    uint32_t moduleRestoreFailures = 0;
    uint32_t frequencyFaults = 0;
    uint32_t packets = 0;
    uint32_t bytes = 0;
    uint32_t emptyWakeups = 0;
    uint32_t ackRequestsIgnored = 0;
    uint32_t telemetryAcksSent = 0;
    uint32_t telemetryRejectedInactive = 0;
    uint32_t telemetryFramesQueued = 0;
    uint32_t telemetryFramesDropped = 0;
    uint32_t v3Frames = 0;
    uint32_t v3TelemetryFrames = 0;
    uint32_t failedTags = 0;
    uint32_t replayFrames = 0;
    uint32_t activationChallenges = 0;
    uint32_t emptyApplicationFrames = 0;
    uint32_t unsupportedProtocolVersions = 0;
    uint32_t unsupportedFrameKinds = 0;
    uint32_t rxFramesQueued = 0;
    uint32_t rxFramesDropped = 0;
    uint32_t commandsQueued = 0;
    uint32_t commandsDropped = 0;
    uint32_t commandsProcessed = 0;
    uint32_t sessionFramesQueued = 0;
    uint32_t sessionFramesDropped = 0;
    uint32_t sessionFramesRejected = 0;
    uint32_t commandResultAcksSent = 0;
    uint32_t commandHintsSent = 0;
    uint32_t powerTargetsSent = 0;
    uint32_t lastPacketMs = 0;
    uint16_t lastSenderId = 0;
    int16_t lastRssi = 0;
};

TaskStats taskStats;
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t detectedVersion = 0;
// Read back from the module by the radio task at every register check.
std::atomic<uint32_t> currentFrequencyHz{0};
uint32_t configuredBitRate = 0;
// What the module read back right after the radio task configured it.
uint8_t expectedRegisters[sizeof(kCheckedRegisters)]{};
std::atomic<bool> expectedRegistersValid{false};
uint32_t lastRegisterCheckMs = 0;

void IRAM_ATTR onRadioInterrupt() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    const uint8_t signal = 1;
    if (interruptQueue != nullptr) {
        xQueueOverwriteFromISR(interruptQueue, &signal, &higherPriorityTaskWoken);
    }
    if (higherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void routeReceivedFrame(ReceivedFrame& received) {
    namespace s = radiosensors::security;
    namespace f = s::frames;
    namespace r = radiosensors::replay;
    namespace n = radiosensors::registry;
    if (received.size == 0 || received.size > sizeof(received.data)) {
        portENTER_CRITICAL(&statsMux);
        ++taskStats.emptyApplicationFrames;
        portEXIT_CRITICAL(&statsMux);
        return;
    }
    const uint8_t header = received.data[0];
    if ((header >> 5) != s::kProtocolMajor) {
        portENTER_CRITICAL(&statsMux);
        ++taskStats.unsupportedProtocolVersions;
        portEXIT_CRITICAL(&statsMux);
        return;
    }
    const bool telemetry = header == 0x60 || header == s::kActivationHeader;
    const bool session = header == f::kCommandReadyHeader || header == f::kCommandResultHeader;
    const bool join = header == f::kJoinRequestHeader || header == f::kJoinConfirmHeader;
    portENTER_CRITICAL(&statsMux);
    ++taskStats.v3Frames;
    if (telemetry) ++taskStats.v3TelemetryFrames;
    if (!telemetry && !session && !join) ++taskStats.unsupportedFrameKinds;
    portEXIT_CRITICAL(&statsMux);
    if (received.targetId != config::nodeId || received.senderId > UINT8_MAX) return;
    if (join) {
        const bool request = header == f::kJoinRequestHeader;
        if (received.control != 0 ||
            (request && (currentProfile.load() != Profile::Commissioning || received.senderId != 0)) ||
            (!request && (currentProfile.load() != Profile::Operational || received.senderId == 0)))
            return;
        received.kind = request ? radiosensors::protocol::FrameKind::JoinRequest
                                : radiosensors::protocol::FrameKind::JoinConfirm;
        const bool queued = xQueueSendToBack(receivedFrameQueue, &received, 0) == pdPASS;
        portENTER_CRITICAL(&statsMux);
        if (queued) ++taskStats.rxFramesQueued;
        else ++taskStats.rxFramesDropped;
        portEXIT_CRITICAL(&statsMux);
        return;
    }
    if ((!telemetry && !session) || currentProfile.load() != Profile::Operational) return;
    n::OpenedFrame opened;
    const auto status = registry_store::receiveSecure(
        {static_cast<uint8_t>(received.targetId), static_cast<uint8_t>(received.senderId), received.control},
        received.data, received.size,
        uxQueueSpacesAvailable(telemetryFrameQueue) != 0,
        uxQueueSpacesAvailable(sessionFrameQueue) != 0, opened);
    if (status != n::ReceiveStatus::Ok) {
        portENTER_CRITICAL(&statsMux);
        if (status == n::ReceiveStatus::FailedTag) ++taskStats.failedTags;
        else if (status == n::ReceiveStatus::Busy) {
            if (telemetry) ++taskStats.telemetryFramesDropped;
            else ++taskStats.sessionFramesDropped;
        } else {
            if (telemetry) ++taskStats.telemetryRejectedInactive;
            else ++taskStats.sessionFramesRejected;
        }
        if (received.ackRequested) ++taskStats.ackRequestsIgnored;
        portEXIT_CRITICAL(&statsMux);
        return;
    }
    const auto action = opened.decision.action;
    const bool accepted = action == r::Action::Accept;
    const bool duplicate = action == r::Action::Duplicate;
    s::Ack ack;
    if (action == r::Action::CounterFloor) {
        ack.hasCounterFloor = true; ack.counterFloor = opened.decision.floor;
    } else if (action == r::Action::Challenge) {
        ack.hasChallenge = true;
        memcpy(ack.challenge, opened.decision.challenge, sizeof(ack.challenge));
    } else if ((accepted || duplicate) && telemetry) {
        ack.commandPending = commands::hasPending(static_cast<uint8_t>(received.senderId));
        if (opened.payloadSize != 0)
            ack.hasPowerTarget = power_control::target(static_cast<uint8_t>(received.senderId),
                                                       opened.payload[0], ack.powerTarget);
    }
    const bool recoveryAck = ack.hasCounterFloor || ack.hasChallenge;
    const bool normalAck = received.ackRequested && (accepted || duplicate) &&
        (telemetry || header == f::kCommandResultHeader);
    if (recoveryAck || normalAck) {
        uint8_t bytes[s::kMaxAckPayloadSize + s::kGatewayTagSize]; size_t size = 0;
        if (s::sealAck(opened.mac, {static_cast<uint8_t>(received.senderId),100,0x80},
                       opened.counter, ack, bytes, sizeof(bytes), size)) {
            rfm69.sendACK(bytes, static_cast<uint8_t>(size));
            portENTER_CRITICAL(&statsMux);
            if (telemetry) ++taskStats.telemetryAcksSent;
            else if (header == f::kCommandResultHeader) ++taskStats.commandResultAcksSent;
            if (ack.commandPending) ++taskStats.commandHintsSent;
            if (ack.hasPowerTarget) ++taskStats.powerTargetsSent;
            portEXIT_CRITICAL(&statsMux);
        }
    } else if (duplicate && opened.replySize != 0) {
        rfm69.send(received.senderId, opened.reply, static_cast<uint8_t>(opened.replySize), false);
    }
    portENTER_CRITICAL(&statsMux);
    if (duplicate || action == r::Action::CounterFloor) ++taskStats.replayFrames;
    if (ack.hasChallenge) ++taskStats.activationChallenges;
    portEXIT_CRITICAL(&statsMux);
    if (!accepted) return;
    received.counter = opened.counter;
    memcpy(received.salt, opened.salt, sizeof(received.salt));
    received.kind = telemetry ? radiosensors::protocol::FrameKind::Telemetry
        : header == f::kCommandReadyHeader ? radiosensors::protocol::FrameKind::CommandReady
                                          : radiosensors::protocol::FrameKind::CommandResult;
    // The application codec consumes its header and unchanged payload layout;
    // the authenticated radio envelope never enters telemetry storage.
    if (telemetry) {
        received.data[0] = radiosensors::protocol::encodeHeader(radiosensors::protocol::FrameKind::Telemetry);
        memcpy(received.data + 1, opened.payload, opened.payloadSize);
        received.size = static_cast<uint8_t>(opened.payloadSize + 1);
    } else {
        memcpy(received.data, opened.payload, opened.payloadSize);
        received.size = static_cast<uint8_t>(opened.payloadSize);
    }
    // The radio task is the sole producer; capacity was checked before the
    // counter was accepted, and consumers can only free space meanwhile.
    const bool queued = xQueueSendToBack(telemetry ? telemetryFrameQueue : sessionFrameQueue, &received, 0) == pdPASS;
    portENTER_CRITICAL(&statsMux);
    if (telemetry) {
        if (queued) ++taskStats.telemetryFramesQueued;
        else ++taskStats.telemetryFramesDropped;
    } else {
        if (queued) ++taskStats.sessionFramesQueued;
        else ++taskStats.sessionFramesDropped;
    }
    portEXIT_CRITICAL(&statsMux);
}

void drainReceivedFrame() {
    if (!rfm69.receiveDone()) {
        portENTER_CRITICAL(&statsMux);
        ++taskStats.emptyWakeups;
        portEXIT_CRITICAL(&statsMux);
        return;
    }
    ReceivedFrame received{};
    received.size = rfm69.DATALEN;
    received.senderId = rfm69.SENDERID; received.targetId = rfm69.TARGETID;
    received.control = rfm69.control; received.rssi = rfm69.RSSI;
    received.ackRequested = rfm69.ACKRequested(); received.receivedAtMs = millis();
    if (received.size <= sizeof(received.data)) memcpy(received.data, rfm69.DATA, received.size);
    portENTER_CRITICAL(&statsMux);
    ++taskStats.packets; taskStats.bytes += received.size;
    taskStats.lastPacketMs = received.receivedAtMs;
    taskStats.lastSenderId = received.senderId; taskStats.lastRssi = received.rssi;
    portEXIT_CRITICAL(&statsMux);
    routeReceivedFrame(received);
    rfm69.receiveDone();
}

// The module keeps its state through an ESP32 reset; without the RESET line
// only a power cycle clears one left mid-reception.
void resetModule() {
    if (config::reset < 0) return;
    pinMode(config::reset, OUTPUT);
    digitalWrite(config::reset, HIGH);
    delayMicroseconds(100);
    digitalWrite(config::reset, LOW);
    delay(5);
}

uint32_t frfToHz(const uint8_t msb, const uint8_t mid, const uint8_t lsb) {
    return static_cast<uint32_t>(
        RF69_FSTEP * ((uint32_t(msb) << 16) | (uint32_t(mid) << 8) | lsb));
}

// The register snapshot cannot catch a wrong carrier: a module that kept its
// reset default of 915 MHz through initialize() was remembered that way and
// then heard nothing. The carrier is therefore compared with the band itself.
bool frequencyConfigured() {
    const uint8_t frf[] = {
        rfm69.readReg(REG_FRFMSB), rfm69.readReg(REG_FRFMID),
        rfm69.readReg(REG_FRFLSB)};
    currentFrequencyHz.store(frfToHz(frf[0], frf[1], frf[2]));
    if (memcmp(frf, kExpectedFrf, sizeof(frf)) == 0) return true;
    portENTER_CRITICAL(&statsMux);
    ++taskStats.frequencyFaults;
    portEXIT_CRITICAL(&statsMux);
    Serial.printf(
        "RFM69 frequency reads %luHz, expected %luHz\n",
        static_cast<unsigned long>(currentFrequencyHz.load()),
        static_cast<unsigned long>(
            frfToHz(kExpectedFrf[0], kExpectedFrf[1], kExpectedFrf[2])));
    return false;
}

uint8_t readCheckedRegister(const uint8_t address) {
    const uint8_t value = rfm69.readReg(address);
    // RestartRx is a trigger, not a setting.
    return address == REG_PACKETCONFIG2
        ? static_cast<uint8_t>(value & ~RF_PACKET2_RXRESTART)
        : value;
}

// Called once the radio task has configured the module and returned it to RX.
void rememberRegisters() {
    expectedRegistersValid.store(false);
    // checkModule() restores a module the library did not leave in RX.
    if (currentState.load() != State::Receiving || RFM69::_mode != RF69_MODE_RX) {
        return;
    }
    for (size_t index = 0; index < sizeof(kCheckedRegisters); ++index) {
        expectedRegisters[index] = readCheckedRegister(kCheckedRegisters[index]);
    }
    expectedRegistersValid.store(true);
}

// initialize() leaves the module at the library's low-power defaults.
void configurePower() {
    rfm69.setHighPower(config::highPower);
    rfm69.setPowerDBm(config::txPowerDbm);
}

void restoreModule() {
    resetModule();
    // setMode() skips the mode the library believes the module is in, so a
    // module that left RX behind its back would never be sent back.
    RFM69::_mode = RF69_MODE_STANDBY;
    const bool restored =
        rfm69.initialize(
            config::frequencyBand, config::nodeId, currentNetworkId.load()) &&
        rfm69.getVersion() == kExpectedVersion &&
        frequencyConfigured();
    if (restored) {
        configurePower();
        rfm69.encrypt(nullptr);
        rfm69.receiveDone();
        rememberRegisters();
    }
    portENTER_CRITICAL(&statsMux);
    if (restored) ++taskStats.moduleRestores;
    else ++taskStats.moduleRestoreFailures;
    portEXIT_CRITICAL(&statsMux);
    Serial.println(restored
        ? "RFM69 restored"
        : "RFM69 restore failed; retrying at the next check");
}

// Reads OpMode's mode bits once the sequencer has finished a transition.
uint8_t settledMode() {
    const uint32_t started = millis();
    while ((rfm69.readReg(REG_IRQFLAGS1) & RF_IRQFLAGS1_MODEREADY) == 0 &&
           millis() - started < kModeReadyTimeoutMs) {}
    return rfm69.readReg(REG_OPMODE) & kOpModeModeMask;
}

void checkModule() {
    if (currentState.load() != State::Receiving) return;
    // Between operations the radio task always leaves the library in RX.
    if (RFM69::_mode != RF69_MODE_RX) {
        Serial.printf("RFM69 left in mode %u instead of RX\n", RFM69::_mode);
        restoreModule();
        return;
    }
    const uint8_t mode = settledMode();
    if (mode != RF_OPMODE_RECEIVER) {
        Serial.printf("RFM69 OpMode mode bits read 0x%02x instead of RX\n", mode);
        restoreModule();
        return;
    }
    if (!frequencyConfigured()) {
        restoreModule();
        return;
    }
    if (!expectedRegistersValid.load()) return;
    for (size_t index = 0; index < sizeof(kCheckedRegisters); ++index) {
        const uint8_t actual = readCheckedRegister(kCheckedRegisters[index]);
        if (actual != expectedRegisters[index]) {
            Serial.printf(
                "RFM69 register 0x%02x reads 0x%02x, expected 0x%02x\n",
                kCheckedRegisters[index], actual, expectedRegisters[index]);
            restoreModule();
            return;
        }
    }
}

void processCommand(const RadioCommand& command) {
    if (command.kind == CommandKind::BeginCommissioning) {

        commissioningEnabled = true;
        rfm69.setNetwork(commissioningNetworkId);
        rfm69.encrypt(nullptr);
        currentProfile.store(Profile::Commissioning);
        currentNetworkId.store(commissioningNetworkId);
    } else if (command.kind == CommandKind::ApplyInstallation) {

        operationalNetworkId = command.networkId;
        rfm69.setNetwork(operationalNetworkId);
        rfm69.encrypt(nullptr);
        currentProfile.store(Profile::Operational);
        currentNetworkId.store(operationalNetworkId);

        commissioningEnabled = false;
        operationalEnabled.store(true);
        currentState.store(State::Receiving);
    } else if (command.kind == CommandKind::SetProfile) {
        if (command.profile == Profile::Commissioning) {
            if (commissioningEnabled) {
                rfm69.setNetwork(commissioningNetworkId);
                rfm69.encrypt(nullptr);
                currentProfile.store(Profile::Commissioning);
                currentNetworkId.store(commissioningNetworkId);
            }
        } else {
            rfm69.setNetwork(operationalNetworkId);
            rfm69.encrypt(nullptr);
            currentProfile.store(Profile::Operational);
            currentNetworkId.store(operationalNetworkId);

            commissioningEnabled = false;
        }
    } else if (command.kind == CommandKind::SessionReply) {
        uint8_t sealed[radiosensors::replay::kMaxReplySize]; size_t size = 0;
        if (registry_store::commandReply(static_cast<uint8_t>(command.targetId), command.salt,
                command.counter, command.header, command.data, command.size, sealed, sizeof(sealed), size))
            rfm69.send(command.targetId, sealed, static_cast<uint8_t>(size), false);
    } else {
        rfm69.send(
            command.targetId,
            command.data,
            command.size,
            command.requestAck);
        if (command.switchAfterSend) {
            if (command.profile == Profile::Commissioning && commissioningEnabled) {
                rfm69.setNetwork(commissioningNetworkId);
                rfm69.encrypt(nullptr);
                currentProfile.store(Profile::Commissioning);
                currentNetworkId.store(commissioningNetworkId);
            } else if (command.profile == Profile::Operational) {
                rfm69.setNetwork(operationalNetworkId);
                rfm69.encrypt(nullptr);
                currentProfile.store(Profile::Operational);
                currentNetworkId.store(operationalNetworkId);

                // Keep the pairing profile available until confirm or timeout.
            }
        }
    }
    rfm69.receiveDone();
    rememberRegisters();
    portENTER_CRITICAL(&statsMux);
    ++taskStats.commandsProcessed;
    portEXIT_CRITICAL(&statsMux);
    if (command.reportCompletion && commandCompletionQueue != nullptr) {
        xQueueSendToBack(commandCompletionQueue, &command.completionId, 0);
    }
}

// Initial setup supplies the operational network before traffic is enabled.
bool acceptsCommands() {
    return commandQueue != nullptr && operationalEnabled.load();
}

bool queueAndWaitForCompletion(RadioCommand& command) {
    if (commandQueue == nullptr || commandCompletionQueue == nullptr ||
        synchronousCommandMutex == nullptr ||
        xSemaphoreTake(
            synchronousCommandMutex, kCommandCompletionTimeout) != pdTRUE) {
        return false;
    }

    uint32_t staleCompletion = 0;
    while (xQueueReceive(commandCompletionQueue, &staleCompletion, 0) == pdPASS) {}
    command.reportCompletion = true;
    command.completionId = nextCompletionId.fetch_add(
        1, std::memory_order_relaxed);
    const bool queued = xQueueSendToBack(commandQueue, &command, 0) == pdPASS;
    if (queued) {
        portENTER_CRITICAL(&statsMux);
        ++taskStats.commandsQueued;
        portEXIT_CRITICAL(&statsMux);
    } else {
        portENTER_CRITICAL(&statsMux);
        ++taskStats.commandsDropped;
        portEXIT_CRITICAL(&statsMux);
    }

    bool result = false;
    const TickType_t started = xTaskGetTickCount();
    while (queued) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= kCommandCompletionTimeout) break;
        uint32_t completedId = 0;
        if (xQueueReceive(
                commandCompletionQueue, &completedId,
                kCommandCompletionTimeout - elapsed) != pdPASS) {
            break;
        }
        if (completedId == command.completionId) {
            result = true;
            break;
        }
    }
    xSemaphoreGive(synchronousCommandMutex);
    return result;
}

void serviceInterrupt() {
    portENTER_CRITICAL(&statsMux);
    ++taskStats.interrupts;
    portEXIT_CRITICAL(&statsMux);
    drainReceivedFrame();
}

// DIO0 signals PayloadReady by a rising edge only. An edge lost to an ESP32
// reset or a race leaves the line high with the frame unread, and the radio
// would never interrupt again.
void serviceMissedInterrupt() {
    if (currentState.load() != State::Receiving || RFM69::_mode != RF69_MODE_RX ||
        digitalRead(config::interrupt) != HIGH) {
        return;
    }
    portENTER_CRITICAL(&statsMux);
    ++taskStats.missedInterrupts;
    portEXIT_CRITICAL(&statsMux);
    rfm69.markInterrupt();
    drainReceivedFrame();
}

void radioTask(void*) {
    for (;;) {
        QueueSetMemberHandle_t ready =
            xQueueSelectFromSet(radioQueueSet, kMissedInterruptCheck);
        uint8_t signal = 0;
        if (ready == nullptr) {
            serviceMissedInterrupt();
        } else if (ready == interruptQueue &&
                   xQueueReceive(interruptQueue, &signal, 0) == pdPASS) {
            serviceInterrupt();
        } else {
            // If RX and a command became ready together, always drain the FIFO first.
            if (xQueueReceive(interruptQueue, &signal, 0) == pdPASS) {
                serviceInterrupt();
            }
            RadioCommand command{};
            if (xQueueReceive(commandQueue, &command, 0) == pdPASS) {
                processCommand(command);
                memset(&command, 0, sizeof(command));
            }
        }
        // Timed here rather than on the idle timeout, which steady traffic
        // would never let expire.
        if (millis() - lastRegisterCheckMs >= kRegisterCheckIntervalMs) {
            lastRegisterCheckMs = millis();
            checkModule();
        }
    }
}

}  // namespace

bool begin() {
    currentState.store(State::Starting);
    const radiosensors::gateway_storage::InstallationSecrets secrets =
        configuration_store::secrets();
    operationalEnabled.store(secrets.installationKeyPresent);
    commissioningEnabled = false;
    operationalNetworkId = secrets.operationalNetworkId;
    commissioningNetworkId = 0;

    currentNetworkId.store(operationalNetworkId);
    pinMode(config::interrupt, INPUT);
    resetModule();

    bool initialized = false;
    for (uint8_t attempt = 1; attempt <= kInitializationAttempts; ++attempt) {
        pinMode(config::chipSelect, OUTPUT);
        digitalWrite(config::chipSelect, HIGH);
        delay(kInitializationRecoveryDelayMs);
        if (!radioSpi.begin(
                config::sck, config::miso, config::mosi,
                config::chipSelect)) {
            Serial.printf("RFM69 SPI initialization attempt %u failed\n", attempt);
        } else if (!rfm69.initialize(
                       config::frequencyBand, config::nodeId,
                       operationalNetworkId)) {
            Serial.printf("RFM69 register probe attempt %u failed\n", attempt);
        } else if (frequencyConfigured()) {
            initialized = true;
            break;
        }
        radioSpi.end();
    }
    if (!initialized) {
        currentState.store(State::InitializationFailed);
        Serial.println("RFM69 initialization failed; gateway will continue without radio");
        return false;
    }

    detectedVersion = rfm69.getVersion();
    if (detectedVersion != kExpectedVersion) {
        currentState.store(State::VersionMismatch);
        rfm69.sleep();
        Serial.printf(
            "RFM69 version mismatch: expected 0x%02x, read 0x%02x\n",
            kExpectedVersion,
            detectedVersion);
        return false;
    }

    configurePower();
    configuredBitRate = rfm69.getBitRate();

    rfm69.encrypt(nullptr);
    if (!operationalEnabled.load()) rfm69.sleep();

    interruptQueue = xQueueCreate(1, sizeof(uint8_t));
    receivedFrameQueue = xQueueCreate(kRxQueueDepth, sizeof(ReceivedFrame));
    telemetryFrameQueue = xQueueCreate(kRxQueueDepth, sizeof(ReceivedFrame));
    sessionFrameQueue = xQueueCreate(kSessionQueueDepth, sizeof(ReceivedFrame));
    commandQueue = xQueueCreate(kCommandQueueDepth, sizeof(RadioCommand));
    commandCompletionQueue = xQueueCreate(kCommandQueueDepth, sizeof(uint32_t));
    synchronousCommandMutex = xSemaphoreCreateMutex();
    radioQueueSet = xQueueCreateSet(1 + kCommandQueueDepth);
    if (interruptQueue == nullptr || receivedFrameQueue == nullptr ||
        telemetryFrameQueue == nullptr || sessionFrameQueue == nullptr ||
        commandQueue == nullptr || commandCompletionQueue == nullptr ||
        synchronousCommandMutex == nullptr || radioQueueSet == nullptr ||
        xQueueAddToSet(interruptQueue, radioQueueSet) != pdPASS ||
        xQueueAddToSet(commandQueue, radioQueueSet) != pdPASS) {
        currentState.store(State::TaskFailed);
        rfm69.sleep();
        Serial.println("RFM69 queue creation failed");
        return false;
    }

    // Keeps the first check out of the rest of begin(), which still uses SPI.
    lastRegisterCheckMs = millis();
    if (xTaskCreatePinnedToCore(
            radioTask,
            "rfm69-rx",
            kTaskStackSize,
            nullptr,
            kTaskPriority,
            &radioTaskHandle,
            kTaskCore) != pdPASS) {
        currentState.store(State::TaskFailed);
        rfm69.sleep();
        Serial.println("RFM69 receive task creation failed");
        return false;
    }

    rfm69.setIsrCallback(onRadioInterrupt);
    if (!operationalEnabled.load()) {
        currentState.store(State::NetworkMissing);
        Serial.println("RFM69 operational network is missing; waiting for initial setup");
        return true;
    }
    rfm69.receiveDone();
    currentState.store(State::Receiving);
    rememberRegisters();

    Serial.printf(
        "RFM69 ready: version=0x%02x frequency=%luHz bitrate=%lubps "
        "variant=%s power=%ddBm SPI=%s pins=%d/%d/%d/%d irq=%d\n",
        detectedVersion,
        currentFrequencyHz.load(),
        configuredBitRate,
        config::highPower ? "HW" : "W",
        config::txPowerDbm,
        spiHostName(),
        config::sck,
        config::miso,
        config::mosi,
        config::chipSelect,
        config::interrupt);
    return true;
}

bool receive(ReceivedFrame& frame, const TickType_t waitTicks) {
    if (receivedFrameQueue == nullptr) {
        if (waitTicks > 0) vTaskDelay(waitTicks);
        return false;
    }
    return xQueueReceive(receivedFrameQueue, &frame, waitTicks) == pdPASS;
}

bool receiveTelemetry(ReceivedFrame& frame, const TickType_t waitTicks) {
    if (telemetryFrameQueue == nullptr) {
        if (waitTicks > 0) vTaskDelay(waitTicks);
        return false;
    }
    return xQueueReceive(telemetryFrameQueue, &frame, waitTicks) == pdPASS;
}

bool receiveSessionFrame(ReceivedFrame& frame, const TickType_t waitTicks) {
    if (sessionFrameQueue == nullptr) {
        if (waitTicks > 0) vTaskDelay(waitTicks);
        return false;
    }
    return xQueueReceive(sessionFrameQueue, &frame, waitTicks) == pdPASS;
}

bool requestProfile(const Profile profile) {
    if (!acceptsCommands() ||
        (profile == Profile::Commissioning && !commissioningEnabled)) {
        return false;
    }
    RadioCommand command{};
    command.kind = CommandKind::SetProfile;
    command.profile = profile;
    return queueAndWaitForCompletion(command);
}

bool beginCommissioning() {
    if (!acceptsCommands()) return false;
    RadioCommand command{};
    command.kind = CommandKind::BeginCommissioning;
    return queueAndWaitForCompletion(command);
}

bool applyInstallation(const uint8_t networkId) {
    if (commandQueue == nullptr || networkId == 0) return false;
    RadioCommand command{};
    command.kind = CommandKind::ApplyInstallation; command.networkId = networkId;
    return queueAndWaitForCompletion(command);
}

bool sendCommandReply(const ReceivedFrame& ready, uint8_t header, const uint8_t* payload, size_t size) {
    if (!acceptsCommands() || size > radiosensors::security::frames::kMaxCommandPayloadSize ||
        (size != 0 && !payload)) return false;
    RadioCommand command{};
    command.kind = CommandKind::SessionReply;
    command.targetId = ready.senderId; command.counter = ready.counter; command.header = header;
    memcpy(command.salt, ready.salt, sizeof(command.salt));
    if (size != 0) memcpy(command.data, payload, size);
    command.size = static_cast<uint8_t>(size);
    const bool queued = xQueueSendToBack(commandQueue, &command, 0) == pdPASS;
    portENTER_CRITICAL(&statsMux);
    if (queued) ++taskStats.commandsQueued;
    else ++taskStats.commandsDropped;
    portEXIT_CRITICAL(&statsMux);
    return queued;
}

bool send(
    const uint16_t targetId,
    const uint8_t* const data,
    const size_t size,
    const bool requestAck) {
    if (!acceptsCommands() || data == nullptr || size > kMaxPayloadSize) {
        return false;
    }
    RadioCommand command{};
    command.kind = CommandKind::Send;
    command.targetId = targetId;
    command.size = static_cast<uint8_t>(size);
    command.requestAck = requestAck;
    for (size_t index = 0; index < size; ++index) command.data[index] = data[index];
    const bool queued = xQueueSendToBack(commandQueue, &command, 0) == pdPASS;
    portENTER_CRITICAL(&statsMux);
    if (queued) ++taskStats.commandsQueued;
    else ++taskStats.commandsDropped;
    portEXIT_CRITICAL(&statsMux);
    return queued;
}

bool sendThenSwitchProfile(
    const uint16_t targetId,
    const uint8_t* const data,
    const size_t size,
    const Profile profile) {
    if (!acceptsCommands() || data == nullptr || size > kMaxPayloadSize ||
        (profile == Profile::Commissioning && !commissioningEnabled)) {
        return false;
    }
    RadioCommand command{};
    command.kind = CommandKind::Send;
    command.targetId = targetId;
    command.size = static_cast<uint8_t>(size);
    command.profile = profile;
    command.switchAfterSend = true;
    for (size_t index = 0; index < size; ++index) command.data[index] = data[index];
    return queueAndWaitForCompletion(command);
}

State state() {
    return currentState.load();
}

const char* stateName() {
    switch (state()) {
        case State::Stopped:
            return "stopped";
        case State::Starting:
            return "starting";
        case State::SpiInitializationFailed:
            return "spi_initialization_failed";
        case State::InitializationFailed:
            return "initialization_failed";
        case State::VersionMismatch:
            return "version_mismatch";
        case State::NetworkMissing:
            return "network_missing";
        case State::TaskFailed:
            return "task_failed";
        case State::Receiving:
            return "receiving";
    }
    return "unknown";
}

const char* frequencyBandName() {
    switch (config::frequencyBand) {
        case RF69_315MHZ:
            return "315";
        case RF69_433MHZ:
            return "433";
        case RF69_868MHZ:
            return "868";
        case RF69_915MHZ:
            return "915";
        default:
            return "unknown";
    }
}

const char* spiHostName() {
    if (config::spiHost == HSPI) {
        return "HSPI";
    }
    if (config::spiHost == FSPI) {
        return "FSPI";
    }
#if defined(VSPI)
    if (config::spiHost == VSPI) {
        return "VSPI";
    }
#endif
    return "unknown";
}

const char* profileName() {
    return currentProfile.load() == Profile::Commissioning
        ? "commissioning"
        : "operational";
}

Snapshot snapshot() {
    Snapshot result{
        state(),
        detectedVersion,
        currentFrequencyHz.load(),
        configuredBitRate,
        config::txPowerDbm,
        operationalEnabled.load(),
        0,
    };

    portENTER_CRITICAL(&statsMux);
    result.interrupts = taskStats.interrupts;
    result.missedInterrupts = taskStats.missedInterrupts;
    result.moduleRestores = taskStats.moduleRestores;
    result.moduleRestoreFailures = taskStats.moduleRestoreFailures;
    result.frequencyFaults = taskStats.frequencyFaults;
    result.packets = taskStats.packets;
    result.bytes = taskStats.bytes;
    result.emptyWakeups = taskStats.emptyWakeups;
    result.ackRequestsIgnored = taskStats.ackRequestsIgnored;
    result.telemetryAcksSent = taskStats.telemetryAcksSent;
    result.telemetryRejectedInactive = taskStats.telemetryRejectedInactive;
    result.telemetryFramesQueued = taskStats.telemetryFramesQueued;
    result.telemetryFramesDropped = taskStats.telemetryFramesDropped;
    result.v3Frames = taskStats.v3Frames;
    result.v3TelemetryFrames = taskStats.v3TelemetryFrames;
    result.failedTags = taskStats.failedTags;
    result.replayFrames = taskStats.replayFrames;
    result.activationChallenges = taskStats.activationChallenges;
    result.emptyApplicationFrames = taskStats.emptyApplicationFrames;
    result.unsupportedProtocolVersions = taskStats.unsupportedProtocolVersions;
    result.unsupportedFrameKinds = taskStats.unsupportedFrameKinds;
    result.rxFramesQueued = taskStats.rxFramesQueued;
    result.rxFramesDropped = taskStats.rxFramesDropped;
    result.commandsQueued = taskStats.commandsQueued;
    result.commandsDropped = taskStats.commandsDropped;
    result.commandsProcessed = taskStats.commandsProcessed;
    result.sessionFramesQueued = taskStats.sessionFramesQueued;
    result.sessionFramesDropped = taskStats.sessionFramesDropped;
    result.sessionFramesRejected = taskStats.sessionFramesRejected;
    result.commandResultAcksSent = taskStats.commandResultAcksSent;
    result.commandHintsSent = taskStats.commandHintsSent;
    result.powerTargetsSent = taskStats.powerTargetsSent;
    result.profile = currentProfile.load();
    result.currentNetworkId = currentNetworkId.load();
    result.lastPacketMs = taskStats.lastPacketMs;
    result.lastSenderId = taskStats.lastSenderId;
    result.lastRssi = taskStats.lastRssi;
    portEXIT_CRITICAL(&statsMux);
    return result;
}

}  // namespace gateway::radio
