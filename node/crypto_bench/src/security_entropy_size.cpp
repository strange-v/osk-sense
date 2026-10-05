#include <Arduino.h>
#include <RadioCrypto.h>
#include <PairingEntropy.h>

namespace e = radiosensors::node::pairing_entropy;
osk::crypto::Cmac mac;
volatile uint8_t sink;
struct SyntheticSamples {
    uint16_t index = 0;
    bool next(uint16_t& capture) {
        if (sink & 1) return false;
        const uint8_t bit = ((index / 2) & 1) ^ (index & 1);
        ++index;
        capture = 7766 + ((sink & 2) ? 0 : bit);
        return true;
    }
};
void setup() {
    // Size-only synthetic source; no hardware entropy or pairing is performed.
    if (sink == 0) return;
    uint8_t factory[16], uid[10], material[8];
    for (uint8_t i = 0; i < sizeof(factory); ++i) factory[i] = sink;
    for (uint8_t i = 0; i < sizeof(uid); ++i) uid[i] = sink;
    SyntheticSamples samples;
    if (!e::collect(samples,material)) return;
    osk::crypto::cmacInit(mac,factory);
    sink = e::condition(mac,uid,sink,material);
}
void loop() {}
