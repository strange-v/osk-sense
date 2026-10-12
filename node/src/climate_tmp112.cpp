#include <Arduino.h>
#include <Wire.h>

#include "CommissioningService.h"
#include "LowPowerClock.h"
#include "NodeRadio.h"
#include "NodeRuntime.h"
#include "ProvisioningButton.h"
#include "Profiles/ClimateTmp112Profile.h"
#include "UnusedPins.h"

namespace {
using namespace radiosensors;
using Profile = node::ClimateTmp112Profile;

// PA5 is the reed pad; PB2/PB3 carry UART only in debug builds.
constexpr uint8_t kUnusedPins[] = {
    PIN_PA5,
#if !defined(NODE_DEBUG)
    PIN_PB2,
    PIN_PB3,
#endif
};

#if defined(NODE_CLIMATE_ADAPTIVE_REPORTING)
const node::ClimateReportPolicy reportPolicy =
    node::ClimateReportPolicy::adaptive(NODE_CLIMATE_CHARGED_REPORT_INTERVAL_MS,
                                        NODE_CLIMATE_LOW_CHARGE_REPORT_INTERVAL_MS,
                                        NODE_CLIMATE_LOW_CHARGE_THRESHOLD_MV);
// Solar-charged: retries follow the report interval, whose low-charge value
// keeps a node without its gateway alive through the night.
node::RadioRetryBackoff radioRetry() {
    return node::RadioRetryBackoff::none();
}
#else
const node::ClimateReportPolicy reportPolicy =
    node::ClimateReportPolicy::fixed(NODE_CLIMATE_REPORT_INTERVAL_MS);
node::RadioRetryBackoff radioRetry() {
    return node::RadioRetryBackoff();
}
#endif

node::NodeRadio radio(NODE_RFM69_CS, NODE_RFM69_IRQ);
node::LowPowerClock clock;
node::ProvisioningButton button(NODE_BUTTON_PIN);
Profile profile(Wire, NODE_TMP112_ADDRESS, reportPolicy);
node::CommissioningService commissioning(radio, Profile::kProfileId);
node::NodeRuntime<Profile> runtime(profile, radio, commissioning, clock, button, radioRetry());
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
