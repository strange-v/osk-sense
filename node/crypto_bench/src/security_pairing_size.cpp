#include <Arduino.h>
#include <EEPROM.h>
#include <RadioSecurityFrames.h>
#include <ArduinoEepromStorage.h>
#include <RadioSecurityPairing.h>

namespace s = radiosensors::security;
radiosensors::node::storage::ArduinoEepromStorage eeprom;
const uint8_t uid[10] = {0,1,2,3,4,5,6,7,8,9};
radiosensors::node::security_pairing::Transaction<decltype(eeprom)> pairing(eeprom,uid);
s::Context context;
uint8_t wire[61];
volatile uint8_t sink;

void setup() {
    // Link EEPROM writes without running them on an idle bench board.
    if (sink == 0) return;
    uint8_t factory[16];
    for (uint8_t i = 0; i < sizeof(factory); ++i) factory[i] = sink;
    sink = pairing.load();
    sink = pairing.begin(sink);
    osk::crypto::cmacInit(context.authentication,factory);
    sink = static_cast<uint8_t>(pairing.accept(context.authentication,s::Transport{0,100,0},wire,35));
    s::initialize(context,factory,pairing.config().salt);
    sink = pairing.confirm(context.authentication,wire,sizeof(wire));
    sink = static_cast<uint8_t>(pairing.complete(context.authentication,
                               s::Transport{pairing.config().nodeId,100,0},wire,25));
}
void loop() {}
