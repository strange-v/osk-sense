#include <Arduino.h>
#include <Wire.h>

#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "NodeRuntime.h"
#include "ProvisioningButton.h"
#include "Profiles/BinarySht40Profile.h"
#include "UnusedPins.h"

namespace {
using namespace radiosensors;
using Profile = node::BinarySht40Profile;

// PB2/PB3 carry UART only in debug builds.
#if !defined(NODE_DEBUG)
constexpr uint8_t kUnusedPins[] = {
    PIN_PB2,
    PIN_PB3,
};
#endif

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::ProvisioningButton button(NODE_BUTTON_PIN);
Profile profile(NODE_REED_PIN, Wire, NODE_SHT40_ADDRESS, NODE_BINARY_CLIMATE_REPORT_INTERVAL_MS);
node::CommissioningService commissioning(radio, Profile::kProfileId);
node::NodeRuntime<Profile> runtime(profile, radio, commissioning, clock, button);
} // namespace

void setup() {
#if defined(NODE_DEBUG)
    Serial.begin(9600);
    Serial.println(F("boot"));
#endif
#if !defined(NODE_DEBUG)
    node::disableUnusedPins(kUnusedPins);
#endif
    runtime.begin();
}

void loop() {
    runtime.runOnce();
}
