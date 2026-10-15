#include "DeviceIdentity.h"

#include <Arduino.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>

#include "ConfigurationStore.h"

namespace gateway::identity {
namespace {

constexpr char kDefaultHostname[] = "osk-hub";

char gatewayHostname[radiosensors::gateway_storage::kHostnameSize + 1] = "osk-hub";
char stableGatewayId[kIdentityCharacters + 1]{};
char currentBootId[kIdentityCharacters + 1]{};

void encodeHex(const uint8_t *bytes, const size_t size, char *output) {
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        output[index * 2] = digits[bytes[index] >> 4];
        output[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    output[size * 2] = '\0';
}

void createGatewayId(const uint8_t mac[6]) {
    static constexpr char domain[] = "radiosensors-gateway-id-v1";
    constexpr size_t domainSize = sizeof(domain) - 1;
    uint8_t material[domainSize + radiosensors::gateway_storage::kDeviceSecretSize]{};
    memcpy(material, domain, domainSize);
    size_t materialSize = domainSize;
    const auto secrets = configuration_store::secrets();
    if (configuration_store::ready() && secrets.deviceSecretPresent) {
        memcpy(material + materialSize,
               secrets.deviceSecret,
               radiosensors::gateway_storage::kDeviceSecretSize);
        materialSize += radiosensors::gateway_storage::kDeviceSecretSize;
    } else {
        memcpy(material + materialSize, mac, 6);
        materialSize += 6;
    }
    uint8_t digest[32]{};
    mbedtls_sha256(material, materialSize, digest, 0);
    encodeHex(digest, kIdentityCharacters / 2, stableGatewayId);
    memset(material, 0, sizeof(material));
    memset(digest, 0, sizeof(digest));
}

} // namespace

// Read once, at boot. The name reaches DNS through mDNS and DHCP through
// ETH.setHostname(), and the latter is only applied while Ethernet starts, so
// a change takes effect on the next restart rather than leaving the two
// disagreeing about what this device is called.
void begin() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        memset(mac, 0, sizeof(mac));
    // An empty setting keeps the default name, so a gateway is reachable at
    // osk-hub.local before anyone has chosen one and after a settings erase.
    snprintf(gatewayHostname, sizeof(gatewayHostname), "%s", kDefaultHostname);
    const auto settings = configuration_store::settings();
    if (settings.hostnameLength != 0) {
        const size_t length = settings.hostnameLength < sizeof(gatewayHostname) - 1
                                  ? settings.hostnameLength
                                  : sizeof(gatewayHostname) - 1;
        memcpy(gatewayHostname, settings.hostname, length);
        gatewayHostname[length] = '\0';
    }
    createGatewayId(mac);
    uint8_t bootBytes[kIdentityCharacters / 2]{};
    esp_fill_random(bootBytes, sizeof(bootBytes));
    encodeHex(bootBytes, sizeof(bootBytes), currentBootId);
    memset(bootBytes, 0, sizeof(bootBytes));
}

const char *hostname() {
    return gatewayHostname;
}

const char *gatewayId() {
    return stableGatewayId;
}

const char *bootId() {
    return currentBootId;
}

} // namespace gateway::identity
