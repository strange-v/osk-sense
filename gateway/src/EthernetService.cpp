#include "EthernetService.h"

#include <ETH.h>
#include <Network.h>
#include <SPI.h>

#include <atomic>

#include "BoardProfile.h"
#include "DeviceIdentity.h"

namespace gateway::ethernet {
namespace {

std::atomic<State> currentState{State::Stopped};

#if defined(GATEWAY_BOARD_WAVESHARE_S3_ETH)
SPIClass ethernetSpi(FSPI);
#endif

void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t) {
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        ETH.setHostname(identity::hostname());
        currentState.store(State::LinkDown);
        Serial.println("Ethernet started");
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        currentState.store(State::LinkUp);
        Serial.println("Ethernet link up");
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        currentState.store(State::Online);
        Serial.printf("Ethernet online: %s\n", ETH.localIP().toString().c_str());
        break;
    case ARDUINO_EVENT_ETH_LOST_IP:
        currentState.store(State::LinkUp);
        Serial.println("Ethernet lost IP address");
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        currentState.store(State::LinkDown);
        Serial.println("Ethernet link down");
        break;
    case ARDUINO_EVENT_ETH_STOP:
        currentState.store(State::Stopped);
        Serial.println("Ethernet stopped");
        break;
    default:
        break;
    }
}

bool startHardware() {
#if defined(GATEWAY_BOARD_WT32_ETH01)
    return ETH.begin(ETH_PHY_LAN8720,
                     board::ethernetPins.phyAddress,
                     board::ethernetPins.mdc,
                     board::ethernetPins.mdio,
                     board::ethernetPins.power,
                     ETH_CLOCK_GPIO0_IN);
#elif defined(GATEWAY_BOARD_WAVESHARE_S3_ETH)
    ethernetSpi.begin(board::ethernetPins.sck,
                      board::ethernetPins.miso,
                      board::ethernetPins.mosi,
                      board::ethernetPins.chipSelect);

    if (!ETH.begin(ETH_PHY_W5500,
                   board::ethernetPins.phyAddress,
                   board::ethernetPins.chipSelect,
                   board::ethernetPins.interrupt,
                   board::ethernetPins.reset,
                   ethernetSpi))
        return false;
    // The W5500 driver opens its socket with IPv4 multicast blocked and lifts
    // the block when lwIP joins a group, but the chip reads that mode only
    // when the socket opens. mDNS queries would then never arrive, so reopen
    // the socket with all multicast already allowed.
    const esp_eth_handle_t handle = ETH.handle();
    bool allMulticast = true;
    return esp_eth_stop(handle) == ESP_OK &&
           esp_eth_ioctl(handle, ETH_CMD_S_ALL_MULTICAST, &allMulticast) == ESP_OK &&
           esp_eth_start(handle) == ESP_OK;
#endif
}

} // namespace

bool begin() {
    currentState.store(State::Starting);
    Network.onEvent(onNetworkEvent);

    if (!startHardware()) {
        currentState.store(State::Failed);
        Serial.println("Ethernet initialization failed");
        return false;
    }

    return true;
}

State state() {
    return currentState.load();
}

const char *stateName() {
    switch (state()) {
    case State::Stopped:
        return "stopped";
    case State::Starting:
        return "starting";
    case State::LinkDown:
        return "link_down";
    case State::LinkUp:
        return "link_up";
    case State::Online:
        return "online";
    case State::Failed:
        return "failed";
    }

    return "unknown";
}

bool hasIp() {
    return state() == State::Online;
}

String ipAddress() {
    return hasIp() ? ETH.localIP().toString() : String();
}

String macAddress() {
    return ETH.macAddress();
}

} // namespace gateway::ethernet
