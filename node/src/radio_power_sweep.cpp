#include <Arduino.h>
#include <ProfileIds.h>
#include <TelemetryFrames.h>

#include "BatteryMonitor.h"
#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "RadioPowerState.h"
#include "UnusedPins.h"

#if NODE_POWER_SWEEP_FIRST_LEVEL > NODE_POWER_SWEEP_LAST_LEVEL
#error "NODE_POWER_SWEEP_FIRST_LEVEL must not exceed the last level"
#endif

#if NODE_POWER_SWEEP_LAST_LEVEL > 31
#error "NODE_POWER_SWEEP_LAST_LEVEL must be in the RFM69 range 0..31"
#endif

#if NODE_POWER_SWEEP_INTERVAL_TICKS < 1
#error "NODE_POWER_SWEEP_INTERVAL_TICKS must be positive"
#endif

namespace {
using namespace radiosensors;

constexpr uint16_t kProfileId =
    protocol::profileIdValue(protocol::ProfileId::Voltage);

// The sweep does not use sensors or UART.
constexpr uint8_t kUnusedPins[] = {
    PIN_PA5,
#if !defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
    PIN_PA6,
#endif
    PIN_PB0,
    PIN_PB1,
    PIN_PB2,
    PIN_PB3,
};

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::BatteryMonitor battery;
node::CommissioningService commissioning(radio, kProfileId);
uint8_t powerLevel = NODE_POWER_SWEEP_FIRST_LEVEL;
#if defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
constexpr uint32_t kReportIntervalMs =
    static_cast<uint32_t>(NODE_POWER_SWEEP_INTERVAL_TICKS) *
    node::LowPowerClock::kTickMs;
uint32_t lastTransmissionAt = 0;
bool buttonDown = false;

void onButtonEdge() {}

void updatePowerLevelFromButton() {
    const bool pressed = digitalRead(NODE_BUTTON_PIN) == LOW;
    if (pressed == buttonDown) return;
    delay(20);
    if ((digitalRead(NODE_BUTTON_PIN) == LOW) != pressed) return;
    buttonDown = pressed;
    if (pressed) {
        powerLevel = powerLevel == NODE_POWER_SWEEP_LAST_LEVEL
            ? NODE_POWER_SWEEP_FIRST_LEVEL
            : powerLevel + 1;
    }
}
#else
uint8_t ticksUntilTransmission = NODE_POWER_SWEEP_INTERVAL_TICKS;
#endif

bool sweepEnabled = false;
#if !defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
bool sweepFinished = false;
#endif

void transmitAtCurrentLevel() {
    radio.setPowerLevel(powerLevel);
    const protocol::TelemetryPrefix prefix{
        battery.readMillivolts(),
        protocol::encodeRadioState(powerLevel, false),
        protocol::kNoDownlinkRssi};
    uint8_t frame[protocol::kVoltageTelemetrySize];
    if (protocol::encodeVoltageTelemetry(prefix, frame, sizeof(frame)) !=
        protocol::TelemetryCodecStatus::Ok) {
        return;
    }

    security::Ack ack;
    int8_t ackRssi = protocol::kNoDownlinkRssi;
    commissioning.sendReport(
        frame,
        static_cast<uint8_t>(sizeof(frame)), node::kTelemetryAttempts, ack,
        ackRssi);
    // Match the production transaction, which samples loaded supply after
    // every transmission even though this diagnostic does not retain it.
    battery.readMillivolts();
}

#if !defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
void transmitNextLevel() {
    transmitAtCurrentLevel();
    if (powerLevel == NODE_POWER_SWEEP_LAST_LEVEL) {
        sweepFinished = true;
    } else {
        ++powerLevel;
    }
}
#endif
}

void setup() {
    node::disableUnusedPins(kUnusedPins);
#if defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
    pinMode(NODE_BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(NODE_BUTTON_PIN), onButtonEdge, CHANGE);
#endif
    clock.begin();
    sweepEnabled = commissioning.begin() == node::StartStatus::Ready &&
        commissioning.active();
}

void loop() {
#if defined(NODE_POWER_SWEEP_BUTTON_LEVEL)
    updatePowerLevelFromButton();
    const uint32_t now = clock.nowMs();
    if (sweepEnabled && now - lastTransmissionAt >= kReportIntervalMs) {
        lastTransmissionAt = now;
        transmitAtCurrentLevel();
    }
    clock.sleepUntilInterrupt();
#else
    clock.sleepUntilInterrupt();
    if (!sweepEnabled || sweepFinished) return;

    if (--ticksUntilTransmission != 0) return;
    ticksUntilTransmission = NODE_POWER_SWEEP_INTERVAL_TICKS;
    transmitNextLevel();
#endif
}
