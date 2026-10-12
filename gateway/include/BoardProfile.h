#pragma once

#include <Arduino.h>

namespace gateway::board {

enum class EthernetController {
    Lan8720,
    W5500,
};

struct Profile {
    const char *name;
    // Board key in a release manifest.
    const char *releaseId;
    EthernetController ethernetController;
    bool hasPoe;
};

struct Lan8720Pins {
    int phyAddress;
    int power;
    int mdc;
    int mdio;
};

struct W5500Pins {
    int phyAddress;
    int sck;
    int miso;
    int mosi;
    int chipSelect;
    int interrupt;
    int reset;
};

#if defined(GATEWAY_BOARD_WT32_ETH01)

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "The WT32-ETH01 profile must be built for ESP32."
#endif

constexpr Profile current{
    "WT32-ETH01",
    "wt32-eth01",
    EthernetController::Lan8720,
    false,
};

// Preserved from the working v1 WT32-ETH01 environment and its board variant.
constexpr Lan8720Pins ethernetPins{
    1,
    16,
    23,
    18,
};

// External button to GND. GPIO32 has an internal pull-up and is not a
// strapping pin, so holding it at power-on does not change the boot mode.
constexpr int buttonPin = 32;

#elif defined(GATEWAY_BOARD_WAVESHARE_S3_ETH)

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "The Waveshare ESP32-S3-ETH profile must be built for ESP32-S3."
#endif

constexpr Profile current{
    "Waveshare ESP32-S3-ETH + PoE",
    "waveshare-s3-eth",
    EthernetController::W5500,
    true,
};

// Waveshare ESP32-S3-ETH schematic and official ETH example pinout.
constexpr W5500Pins ethernetPins{
    1,
    13,
    12,
    11,
    14,
    10,
    9,
};

// The onboard BOOT button; held at power-on it selects the ROM bootloader.
constexpr int buttonPin = 0;

#else
#error "Select a supported gateway board profile."
#endif

} // namespace gateway::board
