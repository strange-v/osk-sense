#include <Arduino.h>

#if defined(WITH_CRYPTO) || defined(WITH_TWO_KEYS) || defined(WITH_CCM)
#include <OskCrypto.h>
#endif

#if defined(WITH_CRYPTO)
osk::crypto::Cmac cmac;
#elif defined(WITH_TWO_KEYS)
osk::crypto::Aes128 encryption;
osk::crypto::Cmac cmac;
#elif defined(WITH_CCM)
osk::crypto::Aes128 aes;
#endif

volatile uint8_t sink[16];

void setup() {
    uint8_t key[16];
    uint8_t frame[16];
    for (uint8_t i = 0; i < 16; ++i) {
        key[i] = sink[i];
        frame[i] = sink[i] ^ 0x5A;
    }
    // What a node does per report: seal an uplink, check a downlink tag.
#if defined(WITH_CRYPTO)
    uint8_t tag[16];
    cmacInit(cmac, key);
    osk::crypto::ctrApply(cmac.aes, key, frame + 7, 9);
    osk::crypto::cmacCompute(cmac, frame, 16, tag);
    for (uint8_t i = 0; i < 16; ++i) sink[i] = tag[i] ^ frame[i];
#elif defined(WITH_TWO_KEYS)
    // Boot derivation from the factory key (here `key`) and an 8-byte salt.
    uint8_t tag[16];
    uint8_t input[9];
    uint8_t encKey[16];
    for (uint8_t i = 0; i < sizeof(input); ++i) input[i] = sink[i];
    cmacInit(cmac, key);
    input[0] = 0x01;
    osk::crypto::cmacCompute(cmac, input, sizeof(input), encKey);
    input[0] = 0x02;
    osk::crypto::cmacCompute(cmac, input, sizeof(input), key);
    osk::crypto::aesExpandKey(encryption, encKey);
    cmacInit(cmac, key);
    osk::crypto::ctrApply(encryption, key, frame + 7, 9);
    osk::crypto::cmacCompute(cmac, frame, 16, tag);
    for (uint8_t i = 0; i < 16; ++i) sink[i] = tag[i] ^ frame[i];
#elif defined(WITH_CCM)
    uint8_t tag[4];
    uint8_t ackTag[4];
    osk::crypto::aesExpandKey(aes, key);
    osk::crypto::ccmSeal(aes, key, 13, frame, 1, frame + 7, 9, tag, 4);
    osk::crypto::ccmSeal(aes, key + 1, 13, frame + 1, 2, nullptr, 0, ackTag, 4);
    for (uint8_t i = 0; i < 4; ++i) sink[i] = tag[i] ^ ackTag[i] ^ frame[i];
#else
    for (uint8_t i = 0; i < 16; ++i) sink[i] = key[i] ^ frame[i];
#endif
}

void loop() {}
