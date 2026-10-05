#include <Arduino.h>

#include "BoardProfile.h"
#include "AuthenticationService.h"
#include "CommandService.h"
#include "CommissioningService.h"
#include "ConfigurationStore.h"
#include "Diagnostics.h"
#include "DeviceIdentity.h"
#include "EthernetService.h"
#include "FirmwareVersion.h"
#include "WebServer.h"
#include "MdnsService.h"
#include "GatewayStatus.h"
#include "NodeRegistryStore.h"
#include "OtaService.h"
#include "PasswordHashService.h"
#include "PowerControlService.h"
#include "RadioService.h"
#include "RecoveryService.h"
#include "TelemetryStore.h"
#include "TimeService.h"
#include "UpdateService.h"
#include "WebUiService.h"

namespace {

constexpr uint32_t kSerialStartupDelayMs = 1000;

const char* ethernetControllerName(gateway::board::EthernetController controller) {
    switch (controller) {
        case gateway::board::EthernetController::Lan8720:
            return "LAN8720 (RMII)";
        case gateway::board::EthernetController::W5500:
            return "W5500 (SPI)";
    }

    return "unknown";
}

}  // namespace

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    // A host that keeps the USB CDC port open without reading it fills the
    // TX buffer, and each write then waits up to 2 s: drop the log rather
    // than stall the gateway.
    Serial.setTxTimeoutMs(0);
#endif
    delay(kSerialStartupDelayMs);

    Serial.println();
    Serial.println("OSK Sense Hub");
    Serial.printf("Firmware: %s\n", gateway::firmware::version);
    Serial.printf("Board: %s\n", gateway::board::current.name);
    Serial.printf(
        "Ethernet: %s\n",
        ethernetControllerName(gateway::board::current.ethernetController));
    Serial.printf("PoE profile: %s\n", gateway::board::current.hasPoe ? "yes" : "no");
    Serial.printf("Reset reason: %s\n", gateway::diagnostics::resetReason());

    gateway::recovery::begin();
    if (!gateway::recovery::blocked()) gateway::configuration_store::begin();
    gateway::identity::begin();
    Serial.printf("Hostname: %s\n", gateway::identity::hostname());
    Serial.printf("Gateway ID: %s\n", gateway::identity::gatewayId());
    Serial.printf("Boot ID: %s\n", gateway::identity::bootId());
    if (!gateway::password_hash::begin()) {
        Serial.println("Password hash worker initialization failed");
    }
    gateway::authentication::begin();
    if (!gateway::recovery::blocked()) gateway::registry_store::begin();
    gateway::telemetry_store::begin();
    gateway::status::begin();
    gateway::ethernet::begin();
    gateway::time_service::begin();
    gateway::mdns_service::begin();
    gateway::web_ui::begin();
    const bool radioReady = !gateway::recovery::blocked() && gateway::radio::begin();
    if (radioReady) {
        gateway::commissioning::begin();
    } else {
        Serial.println("Commissioning disabled because RFM69 is unavailable");
    }
    if (!gateway::recovery::blocked()) gateway::commands::begin(radioReady);
    gateway::web_server::begin();
    gateway::ota::begin();
    gateway::update::begin();

    // Only loop() feeds the watchdog, so startup, which may outlast its 5 s
    // timeout, runs before the loop task is subscribed.
    const bool watchdogStarted = gateway::diagnostics::beginWatchdog();
    Serial.printf("Task watchdog: %s\n", watchdogStarted ? "enabled" : "failed");
}

void loop() {
    gateway::recovery::loop();
    gateway::registry_store::loop();
    gateway::time_service::loop();
    gateway::mdns_service::loop();
    gateway::radio::ReceivedFrame telemetry{};
    for (uint8_t drained = 0;
         drained < 16 && gateway::radio::receiveTelemetry(telemetry);
         ++drained) {
        if (gateway::telemetry_store::accept(telemetry)) {
            gateway::telemetry_store::Record record{};
            if (gateway::telemetry_store::find(
                    static_cast<uint8_t>(telemetry.senderId), record)) {
                gateway::web_server::publishTelemetry(record);
                gateway::power_control::observe(record);
            }
            Serial.printf(
                "Telemetry stored: sender=%u bytes=%u rssi=%d\n",
                telemetry.senderId,
                telemetry.size,
                telemetry.rssi);
        } else {
            Serial.printf(
                "Telemetry store rejected frame: sender=%u bytes=%u\n",
                telemetry.senderId,
                telemetry.size);
        }
    }
    gateway::status::loop();
    gateway::web_server::loop();
    gateway::authentication::loop();
    gateway::ota::loop();
    gateway::update::loop();
    gateway::diagnostics::feedWatchdog();
    delay(100);
}
