#include "MdnsService.h"

#include <ESPmDNS.h>
#include <GatewayStream.h>

#include "ApiVersion.h"
#include "BoardProfile.h"
#include "ConfigurationStore.h"
#include "DeviceIdentity.h"
#include "EthernetService.h"
#include "FirmwareVersion.h"

namespace gateway::mdns_service {
namespace {

bool active = false;
uint32_t appliedGeneration = UINT32_MAX;
uint32_t retryAt = 0;

void stop() {
    if (!active)
        return;
    MDNS.end();
    active = false;
    Serial.println("mDNS stopped");
}

void apply() {
    const auto settings = configuration_store::settings();
    const uint32_t generation = configuration_store::settingsGeneration();
    if (!settings.mdnsEnabled || !ethernet::hasIp()) {
        stop();
        appliedGeneration = generation;
        return;
    }
    if (active && appliedGeneration == generation)
        return;
    if (!active && appliedGeneration == generation && static_cast<int32_t>(millis() - retryAt) < 0)
        return;
    stop();
    if (!MDNS.begin(identity::hostname())) {
        Serial.println("mDNS start failed");
        appliedGeneration = generation;
        retryAt = millis() + 5000;
        return;
    }
    active = true;
    appliedGeneration = generation;
    retryAt = 0;
    // Without storage the name still resolves, so the recovery page is
    // reachable, but the service stays unannounced: its gateway_id would be
    // the MAC-derived fallback, which discovery would offer as a new gateway.
    if (!configuration_store::ready()) {
        Serial.printf("mDNS resolving %s.local, service not announced\n", identity::hostname());
        return;
    }
    MDNS.addService("osk-sense", "tcp", 80);
    MDNS.addServiceTxt("osk-sense", "tcp", "api", String(api::version));
    MDNS.addServiceTxt("osk-sense", "tcp", "api_version", String(api::version));
    MDNS.addServiceTxt("osk-sense", "tcp", "gateway_id", identity::gatewayId());
    MDNS.addServiceTxt("osk-sense", "tcp", "boot_id", identity::bootId());
    MDNS.addServiceTxt("osk-sense",
                       "tcp",
                       "stream_version",
                       String(radiosensors::stream::kVersion));
    MDNS.addServiceTxt("osk-sense", "tcp", "firmware", firmware::version);
    MDNS.addServiceTxt("osk-sense", "tcp", "board", board::current.name);
    MDNS.addServiceTxt("osk-sense", "tcp", "hostname", identity::hostname());
    Serial.printf("mDNS advertising %s.local\n", identity::hostname());
}

} // namespace

void begin() {
    apply();
}
void loop() {
    apply();
}

} // namespace gateway::mdns_service
