#include <Arduino.h>
#include <ProfileIds.h>
#include <TelemetryFrames.h>

#include "BatteryMonitor.h"
#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "RadioPowerState.h"
#include "ProvisioningButton.h"
#include "UnusedPins.h"
#include "WatchdogWindow.h"

#if NODE_FLOOD_POWER_LEVEL > NODE_RADIO_MAX_POWER_LEVEL
#error "NODE_FLOOD_POWER_LEVEL must not exceed NODE_RADIO_MAX_POWER_LEVEL"
#endif

namespace {
using namespace radiosensors;

constexpr uint16_t kProfileId =
    protocol::profileIdValue(protocol::ProfileId::Voltage);
constexpr uint16_t kSummaryFrames = 100;

// PB2/PB3 carry the UART summary.
constexpr uint8_t kUnusedPins[] = {
    PIN_PA5,
    PIN_PB0,
    PIN_PB1,
};

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::BatteryMonitor battery;
node::ProvisioningButton button(NODE_BUTTON_PIN);
node::CommissioningService commissioning(radio, kProfileId);
bool radioReady = false;
uint16_t sent = 0;
uint16_t acknowledged = 0;
uint16_t powerTargets = 0;
uint32_t windowStartedAt = 0;

void restart() {
    Serial.flush();
    _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRE_bm);
    while (true) {}
}

// The level stays fixed: the gateway's power targets are counted, not
// applied, so every frame costs the same airtime.
void transmit() {
    const protocol::TelemetryPrefix prefix{
        battery.readMillivolts(),
        protocol::encodeRadioState(NODE_FLOOD_POWER_LEVEL, false),
        protocol::kNoDownlinkRssi};
    uint8_t frame[protocol::kVoltageTelemetrySize];
    if (protocol::encodeVoltageTelemetry(prefix, frame, sizeof(frame)) !=
        protocol::TelemetryCodecStatus::Ok) {
        return;
    }
    security::Ack ack;
    int8_t ackRssi = protocol::kNoDownlinkRssi;
    node::WatchdogWindow watchdog;
    if (commissioning.sendReport(
            frame,
            static_cast<uint8_t>(sizeof(frame)), node::kTelemetryAttempts,
            ack, ackRssi)) {
        ++acknowledged;
        if (ack.hasPowerTarget) ++powerTargets;
    }
    ++sent;
}

// Power-down stops the UART clock: a line still in the buffer would come out
// as garbage.
void sleep() {
    Serial.flush();
    clock.sleepUntilInterrupt();
}

void printSummary() {
    const uint32_t now = millis();
    Serial.print(F("sent="));
    Serial.print(sent);
    Serial.print(F(" ack="));
    Serial.print(acknowledged);
    Serial.print(F(" tgt="));
    Serial.print(powerTargets);
    Serial.print(F(" ms="));
    Serial.println(now - windowStartedAt);
    sent = 0;
    acknowledged = 0;
    powerTargets = 0;
    windowStartedAt = now;
}
}  // namespace

void setup() {
    Serial.begin(9600);
    Serial.println(F("boot flood"));
    node::disableUnusedPins(kUnusedPins);
    button.begin();
    clock.begin();
    {
        node::WatchdogWindow watchdog;
        radioReady = commissioning.begin() == node::StartStatus::Ready;
    }
    if (radioReady && commissioning.active()) {
        radio.setPowerLevel(NODE_FLOOD_POWER_LEVEL);
    }
    Serial.println(radioReady ? F("rf ok") : F("rf fail"));
}

void loop() {
    const node::ButtonGesture gesture = button.takeGesture();
    if (gesture == node::ButtonGesture::LongPress &&
        commissioning.resetNetwork()) {
        Serial.println(F("rst"));
        restart();
    }
    if (!radioReady) {
        sleep();
        return;
    }
    if (!commissioning.active()) {
        if (gesture == node::ButtonGesture::ShortPress) {
            node::WatchdogWindow watchdog;
            const bool joined = commissioning.advance();
            if (joined) radio.setPowerLevel(NODE_FLOOD_POWER_LEVEL);
            Serial.println(joined ? F("join ok") : F("join fail"));
            windowStartedAt = millis();
        }
        sleep();
        return;
    }

    transmit();
    if (sent == kSummaryFrames) printSummary();
#if NODE_FLOOD_INTERVAL_MS > 0
    delay(NODE_FLOOD_INTERVAL_MS);
#endif
}
