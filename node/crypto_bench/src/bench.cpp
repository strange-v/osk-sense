#include <Arduino.h>
#include <CryptoSelfTest.h>
#include <OskCrypto.h>

using namespace osk::crypto;

namespace {

constexpr uint16_t kRuns = 64;
Cmac cmac;
Aes128 encryption;
uint8_t nonce[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
uint8_t aad[2] = {0x41, 0x02};
uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
uint8_t factoryKey[16] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30};
uint8_t salt[8] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58};
uint8_t block[32];
uint8_t tag[16];

uint32_t emptyLoop() {
    const uint32_t start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) __asm__ __volatile__("" ::: "memory");
    return micros() - start;
}

void report(const __FlashStringHelper* name, const uint32_t total) {
    const uint32_t overhead = emptyLoop();
    const uint32_t net = total > overhead ? total - overhead : 0;
    Serial.print(name);
    Serial.print(F(": "));
    Serial.print(net / kRuns);
    Serial.print(F(" us, "));
    Serial.print(net * (F_CPU / 1000000UL) / kRuns);
    Serial.println(F(" cycles"));
}

}  // namespace

void runBench() {
    Serial.print(F("F_CPU "));
    Serial.println(F_CPU);
    const uint16_t failed = selfTest();
    Serial.print(F("self-test mask 0x"));
    Serial.print(failed, HEX);
    Serial.println(failed ? F(" FAIL") : F(" PASS"));

    uint32_t start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) aesExpandKey(cmac.aes, key);
    report(F("key expansion"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) aesEncrypt(cmac.aes, block);
    report(F("AES block"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) cmacInit(cmac, key);
    report(F("CMAC init"), micros() - start);

    // One report: CTR over a 9-byte payload, then a tag over 16 bytes of
    // header and ciphertext.
    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        ctrApply(cmac.aes, key, block + 7, 9);
        cmacCompute(cmac, block, 16, tag);
    }
    report(F("seal uplink"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) cmacCompute(cmac, block, 8, tag);
    report(F("verify ACK"), micros() - start);

    // Boot: derive K_enc and K_mac from the factory key and the salt, then
    // expand both. The CMAC context serves the derivation first, so the
    // derivation needs no RAM of its own.
    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        uint8_t input[9];
        uint8_t encKey[16];
        uint8_t macKey[16];
        memcpy(input + 1, salt, sizeof(salt));
        cmacInit(cmac, factoryKey);
        input[0] = 0x01;
        cmacCompute(cmac, input, sizeof(input), encKey);
        input[0] = 0x02;
        cmacCompute(cmac, input, sizeof(input), macKey);
        aesExpandKey(encryption, encKey);
        cmacInit(cmac, macKey);
    }
    report(F("boot key derivation and setup"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) cmacCompute(cmac, block, 24, tag);
    report(F("tag over 17..32 bytes"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        ctrApply(encryption, key, block + 7, 9);
        cmacCompute(cmac, block, 16, tag);
    }
    report(F("two-key seal uplink"), micros() - start);

    // CCM, one key: a 1-byte header as associated data and a 9-byte payload;
    // the ACK authenticates 2 bytes of associated data with no payload.
    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        ccmSeal(cmac.aes, nonce, 13, aad, 1, block + 7, 9, tag, 4);
    }
    report(F("CCM seal uplink"), micros() - start);

    start = micros();
    for (uint16_t i = 0; i < kRuns; ++i) {
        ccmSeal(cmac.aes, nonce, 13, aad, 2, nullptr, 0, tag, 4);
    }
    report(F("CCM verify ACK"), micros() - start);
}

void setup() { Serial.begin(9600); }

// Repeats so a monitor opened after the upload still sees a full report.
void loop() {
    runBench();
    Serial.println();
    delay(3000);
}
