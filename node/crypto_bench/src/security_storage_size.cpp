#include <Arduino.h>
#include <EEPROM.h>
#include <ArduinoEepromStorage.h>
#include <RadioSecurityStorage.h>

namespace secure = radiosensors::node::security_storage;
radiosensors::node::storage::ArduinoEepromStorage eeprom;
secure::NetworkConfigStore<decltype(eeprom)> configStore(eeprom);
secure::FrameCounterStore<decltype(eeprom)> counterStore(eeprom);
volatile uint8_t sink;

void setup() {
    // Keep the EEPROM paths linked without changing a bench board's records.
    if (sink == 0) return;
    secure::NetworkConfig config;
    const bool configured = configStore.load(config);
    counterStore.load(configured);
    uint32_t counter = 0;
    sink = counterStore.take(counter);
    sink = counterStore.advanceToFloor(counter, counter + 256);
    if (sink & 1) {
        config.nodeId = 7;
        config.gatewayId = 100;
        config.networkId = sink;
        config.requestNonce = counter;
        for (uint8_t i = 0; i < 8; ++i) config.salt[i] = sink + i;
        sink = configStore.save(config);
    }
    if (sink & 2) sink = configStore.factoryReset();
}
void loop() {}
