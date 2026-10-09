// Bench image for gateway replay and tag checks over the air. An active node
// sends two valid reports, then repeats and tampers with their exact bytes.
// Gateway counters show the outcome; see README.md#radio_replay.
#include <Arduino.h>
#include <ProfileIds.h>
#include <RadioSecurity.h>
#include <TelemetryFrames.h>

#include "BatteryMonitor.h"
#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "ProvisioningButton.h"
#include "UnusedPins.h"
#include "WatchdogWindow.h"

namespace {
using namespace radiosensors;

constexpr uint16_t kProfileId =
    protocol::profileIdValue(protocol::ProfileId::BinaryClimateTh);
constexpr uint8_t kGatewayId = 100;
constexpr uint8_t kReportHeader = 0x60;
constexpr uint8_t kRequestAck = 0x40;

constexpr uint8_t kUnusedPins[] = {PIN_PA5, PIN_PB0, PIN_PB1};

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::BatteryMonitor battery;
node::ProvisioningButton button(NODE_BUTTON_PIN);
node::CommissioningService commissioning(radio, kProfileId);
bool radioReady = false;

struct Sealed {
    uint8_t wire[security::kMaxWireSize];
    uint8_t size = 0;
    uint32_t counter = 0;
};

// Marker values (state 1, 20.00 C, 50.00 %) identify bench reports in the UI.
bool seal(Sealed& frame) {
    const protocol::TelemetryPrefix prefix{
        battery.readMillivolts(),
        protocol::encodeRadioState(NODE_REPLAY_POWER_LEVEL, false),
        protocol::kNoDownlinkRssi};
    uint8_t telemetry[protocol::kBinaryClimateThTelemetrySize];
    if (protocol::encodeBinaryClimateThTelemetry(
            prefix, 1, 2000, 5000, telemetry, sizeof(telemetry)) !=
        protocol::TelemetryCodecStatus::Ok) {
        return false;
    }
    size_t size = 0;
    if (!commissioning.sealFrame(
            kReportHeader, telemetry + 1, sizeof(telemetry) - 1, kRequestAck,
            frame.wire, sizeof(frame.wire), size, frame.counter)) {
        return false;
    }
    frame.size = static_cast<uint8_t>(size);
    return true;
}

void printAck(const bool acknowledged, const security::Ack& ack) {
    if (!acknowledged) {
        Serial.println(F("no ack"));
        return;
    }
    Serial.print(F("ack"));
    if (ack.hasCounterFloor) {
        Serial.print(F(" floor="));
        Serial.print(ack.counterFloor);
    }
    if (ack.hasPowerTarget) {
        Serial.print(F(" tgt="));
        Serial.print(ack.powerTarget);
    }
    if (ack.hasChallenge) Serial.print(F(" challenge"));
    Serial.println();
}

// One transmission of the given bytes; the tag is checked under `counter`.
void exchange(const __FlashStringHelper* step, const uint8_t* wire,
              const uint8_t size, const uint32_t counter) {
    Serial.print(step);
    Serial.print(F(" c="));
    Serial.print(counter);
    Serial.print(F(": "));
    security::Ack ack;
    int8_t rssi = protocol::kNoDownlinkRssi;
    const bool acknowledged = radio.sendAcknowledged(
        kGatewayId, wire, size, 1, commissioning.context().authentication,
        counter, ack, rssi);
    printAck(acknowledged, ack);
    delay(200);
}

void flipped(const __FlashStringHelper* step, const Sealed& frame,
             const uint8_t index) {
    uint8_t copy[sizeof(frame.wire)];
    memcpy(copy, frame.wire, frame.size);
    copy[index] ^= 0x01;
    exchange(step, copy, frame.size, frame.counter);
}

void runScenario() {
    node::WatchdogWindow watchdog;
    if (!radio.ensureConfigured()) {
        Serial.println(F("rf lost"));
        return;
    }
    radio.setPowerLevel(NODE_REPLAY_POWER_LEVEL);
    Sealed a, b;
    if (!seal(a)) {
        Serial.println(F("seal fail"));
        return;
    }
    exchange(F("1 A"), a.wire, a.size, a.counter);
    if (!seal(b)) {
        Serial.println(F("seal fail"));
        return;
    }
    exchange(F("1 B"), b.wire, b.size, b.counter);
    exchange(F("2 B again"), b.wire, b.size, b.counter);
    exchange(F("3 A stale"), a.wire, a.size, a.counter);
    // Wire: header, counter (4), ciphertext, tag (8).
    flipped(F("4 B ciphertext"), b, 5);
    flipped(F("5 B tag"), b, b.size - 1);
    flipped(F("6 B counter"), b, 1);

    // The tag covers the transport control byte: same bytes, no ACK request.
    Serial.print(F("7 B no-ack control c="));
    Serial.print(b.counter);
    Serial.println(F(": sent"));
    radio.send(kGatewayId, b.wire, b.size);
    delay(200);

    // The tag covers the sender: same bytes from another node ID.
    const uint8_t nodeId = commissioning.config().nodeId;
    const uint8_t networkId = commissioning.config().networkId;
    radio.useProfile(static_cast<uint8_t>(nodeId + 1), networkId);
    radio.setPowerLevel(NODE_REPLAY_POWER_LEVEL);
    exchange(F("8 B other sender"), b.wire, b.size, b.counter);
    radio.useProfile(nodeId, networkId);
    radio.setPowerLevel(NODE_REPLAY_POWER_LEVEL);
    radio.sleep();
    Serial.println(F("done"));
}
}  // namespace

void setup() {
    Serial.begin(9600);
    Serial.println(F("boot replay"));
    node::disableUnusedPins(kUnusedPins);
    button.begin();
    clock.begin();
    {
        node::WatchdogWindow watchdog;
        radioReady = commissioning.begin() == node::StartStatus::Ready;
    }
    Serial.println(radioReady && commissioning.active()
        ? F("rf ok, press PA6 to run")
        : F("needs an active node"));
}

void loop() {
    if (radioReady && commissioning.active() &&
        button.takeGesture() == node::ButtonGesture::ShortPress) {
        runScenario();
    }
    Serial.flush();
    clock.sleepUntilInterrupt();
}
