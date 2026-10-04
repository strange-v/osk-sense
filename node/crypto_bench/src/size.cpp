#include <Arduino.h>

#if defined(WITH_CRYPTO)
#include <OskCrypto.h>
osk::crypto::Cmac cmac;
#endif

volatile uint8_t sink[16];

void setup() {
    uint8_t key[16];
    uint8_t frame[16];
    for (uint8_t i = 0; i < 16; ++i) {
        key[i] = sink[i];
        frame[i] = sink[i] ^ 0x5A;
    }
#if defined(WITH_CRYPTO)
    // What a node does per report: seal an uplink, check a downlink tag.
    uint8_t tag[16];
    cmacInit(cmac, key);
    osk::crypto::ctrApply(cmac.aes, key, frame + 7, 9);
    osk::crypto::cmacCompute(cmac, frame, 16, tag);
    for (uint8_t i = 0; i < 16; ++i) sink[i] = tag[i] ^ frame[i];
#else
    for (uint8_t i = 0; i < 16; ++i) sink[i] = key[i] ^ frame[i];
#endif
}

void loop() {}
