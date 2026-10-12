#include <Arduino.h>

#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "NodeRuntime.h"
#include "ProvisioningButton.h"
#include "Profiles/BinaryInputProfile.h"
#include "UnusedPins.h"

namespace {
using namespace radiosensors;
using Profile = node::BinaryInputProfile;

// PB0/PB1 are the unpopulated sensor I2C pads; PB2/PB3 carry UART only in
// debug builds.
constexpr uint8_t kUnusedPins[] = {
    PIN_PB0,
    PIN_PB1,
#if !defined(NODE_DEBUG)
    PIN_PB2,
    PIN_PB3,
#endif
};

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::ProvisioningButton button(NODE_BUTTON_PIN);
Profile profile(NODE_REED_PIN);
node::CommissioningService commissioning(radio, Profile::kProfileId);
node::NodeRuntime<Profile> runtime(profile, radio, commissioning, clock, button);
} // namespace

void setup() {
#if defined(NODE_DEBUG)
    Serial.begin(9600);
    Serial.println(F("boot"));
#endif
    node::disableUnusedPins(kUnusedPins);
    runtime.begin();
}

void loop() {
    runtime.runOnce();
}
